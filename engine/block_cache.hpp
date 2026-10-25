#pragma once

#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include "slice.hpp"

namespace lsm {

struct CacheHandle {
    std::string key;
    std::shared_ptr<std::string> value;
    size_t charge{0};
    uint32_t ref_count{1};
    bool in_cache{true};
    CacheHandle* next{nullptr};
    CacheHandle* prev{nullptr};
};

// -----------------------------------------------------------------------------
// LRUCacheShard: Single lock domain managing LRU doubly-linked list & table
// -----------------------------------------------------------------------------
class LRUCacheShard {
public:
    LRUCacheShard();
    ~LRUCacheShard();

    void SetCapacity(size_t capacity);

    CacheHandle* Insert(const Slice& key, std::shared_ptr<std::string> value, size_t charge);
    CacheHandle* Lookup(const Slice& key);
    void Release(CacheHandle* handle);
    void Erase(const Slice& key);

    size_t TotalCharge() const;
    size_t Hits() const { return hits_.load(std::memory_order_relaxed); }
    size_t Misses() const { return misses_.load(std::memory_order_relaxed); }

private:
    void LRU_Remove(CacheHandle* e);
    void LRU_Append(CacheHandle* e);
    void Unref(CacheHandle* e);

    mutable std::mutex mu_;
    size_t capacity_{0};
    size_t usage_{0};

    // Dummy head for circular doubly-linked LRU list
    CacheHandle lru_head_;
    std::unordered_map<std::string, CacheHandle*> table_;

    std::atomic<size_t> hits_{0};
    std::atomic<size_t> misses_{0};
};

// -----------------------------------------------------------------------------
// ShardedLRUCache: High-concurrency 16-shard LRU Block Cache
// Eliminates lock contention across concurrent multi-worker compaction & query threads
// -----------------------------------------------------------------------------
class ShardedLRUCache {
public:
    static constexpr size_t kNumShards = 16;

    explicit ShardedLRUCache(size_t total_capacity_bytes = 16 * 1024 * 1024);
    ~ShardedLRUCache() = default;

    ShardedLRUCache(const ShardedLRUCache&) = delete;
    ShardedLRUCache& operator=(const ShardedLRUCache&) = delete;

    using Handle = CacheHandle;

    Handle* Insert(const Slice& key, std::shared_ptr<std::string> value, size_t charge);
    Handle* Lookup(const Slice& key);
    void Release(Handle* handle);
    void Erase(const Slice& key);

    Slice Value(Handle* handle) const;

    size_t TotalCharge() const;
    size_t Capacity() const { return total_capacity_; }
    size_t Hits() const;
    size_t Misses() const;
    double HitRatio() const;

private:
    static uint32_t HashSlice(const Slice& s);
    static size_t Shard(uint32_t hash) { return hash & (kNumShards - 1); }

    size_t total_capacity_{0};
    std::vector<std::unique_ptr<LRUCacheShard>> shards_;
};

} // namespace lsm
