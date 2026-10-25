#include "block_cache.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// LRUCacheShard Implementation
// -----------------------------------------------------------------------------
LRUCacheShard::LRUCacheShard() {
    lru_head_.next = &lru_head_;
    lru_head_.prev = &lru_head_;
}

LRUCacheShard::~LRUCacheShard() {
    for (auto it = table_.begin(); it != table_.end(); ++it) {
        CacheHandle* e = it->second;
        e->in_cache = false;
        Unref(e);
    }
    table_.clear();
}

void LRUCacheShard::SetCapacity(size_t capacity) {
    std::lock_guard<std::mutex> lock(mu_);
    capacity_ = capacity;
    while (usage_ > capacity_ && lru_head_.next != &lru_head_) {
        CacheHandle* oldest = lru_head_.next;
        if (oldest->ref_count <= 1) {
            LRU_Remove(oldest);
            table_.erase(oldest->key);
            oldest->in_cache = false;
            usage_ -= oldest->charge;
            Unref(oldest);
        } else {
            break;
        }
    }
}

void LRUCacheShard::LRU_Remove(CacheHandle* e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
}

void LRUCacheShard::LRU_Append(CacheHandle* e) {
    // Append to most-recently-used position (before lru_head_)
    e->next = &lru_head_;
    e->prev = lru_head_.prev;
    e->prev->next = e;
    lru_head_.prev = e;
}

void LRUCacheShard::Unref(CacheHandle* e) {
    if (--e->ref_count == 0) {
        delete e;
    }
}

CacheHandle* LRUCacheShard::Lookup(const Slice& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = table_.find(key.ToString());
    if (it != table_.end()) {
        CacheHandle* e = it->second;
        e->ref_count++;
        // Move to MRU position
        LRU_Remove(e);
        LRU_Append(e);
        hits_.fetch_add(1, std::memory_order_relaxed);
        return e;
    }
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

CacheHandle* LRUCacheShard::Insert(
    const Slice& key,
    std::shared_ptr<std::string> value,
    size_t charge
) {
    std::lock_guard<std::mutex> lock(mu_);

    auto e = new CacheHandle();
    e->key = key.ToString();
    e->value = std::move(value);
    e->charge = charge;
    e->ref_count = 2; // 1 for returned handle lease + 1 for cache table
    e->in_cache = true;

    auto it = table_.find(e->key);
    if (it != table_.end()) {
        CacheHandle* old = it->second;
        LRU_Remove(old);
        old->in_cache = false;
        usage_ -= old->charge;
        Unref(old);
    }

    table_[e->key] = e;
    LRU_Append(e);
    usage_ += charge;

    // Evict oldest unpinned entries if usage exceeds capacity
    while (usage_ > capacity_ && lru_head_.next != &lru_head_) {
        CacheHandle* oldest = lru_head_.next;
        if (oldest->ref_count <= 1) { // Not pinned by caller
            LRU_Remove(oldest);
            table_.erase(oldest->key);
            oldest->in_cache = false;
            usage_ -= oldest->charge;
            Unref(oldest);
        } else {
            // All available cache elements are pinned
            break;
        }
    }

    return e;
}

void LRUCacheShard::Release(CacheHandle* handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lock(mu_);
    Unref(handle);
}

void LRUCacheShard::Erase(const Slice& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = table_.find(key.ToString());
    if (it != table_.end()) {
        CacheHandle* e = it->second;
        LRU_Remove(e);
        table_.erase(it);
        e->in_cache = false;
        usage_ -= e->charge;
        Unref(e);
    }
}

size_t LRUCacheShard::TotalCharge() const {
    std::lock_guard<std::mutex> lock(mu_);
    return usage_;
}

// -----------------------------------------------------------------------------
// ShardedLRUCache Implementation
// -----------------------------------------------------------------------------
ShardedLRUCache::ShardedLRUCache(size_t total_capacity_bytes)
    : total_capacity_(total_capacity_bytes) {
    shards_.reserve(kNumShards);
    size_t per_shard_cap = (total_capacity_bytes + kNumShards - 1) / kNumShards;
    for (size_t i = 0; i < kNumShards; ++i) {
        auto shard = std::make_unique<LRUCacheShard>();
        shard->SetCapacity(per_shard_cap);
        shards_.push_back(std::move(shard));
    }
}

uint32_t ShardedLRUCache::HashSlice(const Slice& s) {
    // MurmurHash2 32-bit hash implementation
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    uint32_t h = static_cast<uint32_t>(s.size()) ^ 0xbc9f1d34;
    const unsigned char* data = reinterpret_cast<const unsigned char*>(s.data());
    size_t len = s.size();

    while (len >= 4) {
        uint32_t k = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
        data += 4;
        len -= 4;
    }

    switch (len) {
        case 3: h ^= data[2] << 16; [[fallthrough]];
        case 2: h ^= data[1] << 8; [[fallthrough]];
        case 1: h ^= data[0]; h *= m;
    }

    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}

CacheHandle* ShardedLRUCache::Insert(
    const Slice& key,
    std::shared_ptr<std::string> value,
    size_t charge
) {
    uint32_t hash = HashSlice(key);
    return shards_[Shard(hash)]->Insert(key, std::move(value), charge);
}

CacheHandle* ShardedLRUCache::Lookup(const Slice& key) {
    uint32_t hash = HashSlice(key);
    return shards_[Shard(hash)]->Lookup(key);
}

void ShardedLRUCache::Release(Handle* handle) {
    if (!handle) return;
    uint32_t hash = HashSlice(handle->key);
    shards_[Shard(hash)]->Release(handle);
}

void ShardedLRUCache::Erase(const Slice& key) {
    uint32_t hash = HashSlice(key);
    shards_[Shard(hash)]->Erase(key);
}

Slice ShardedLRUCache::Value(Handle* handle) const {
    if (!handle || !handle->value) return Slice();
    return Slice(*handle->value);
}

size_t ShardedLRUCache::TotalCharge() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->TotalCharge();
    }
    return total;
}

size_t ShardedLRUCache::Hits() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->Hits();
    }
    return total;
}

size_t ShardedLRUCache::Misses() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->Misses();
    }
    return total;
}

double ShardedLRUCache::HitRatio() const {
    size_t h = Hits();
    size_t m = Misses();
    if (h + m == 0) return 1.0;
    return static_cast<double>(h) / static_cast<double>(h + m);
}

} // namespace lsm
