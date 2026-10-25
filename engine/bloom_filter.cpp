#include "bloom_filter.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace lsm {

namespace {

static inline void EncodeFixed32(char* buf, uint32_t value) {
    uint8_t* const buffer = reinterpret_cast<uint8_t*>(buf);
    buffer[0] = static_cast<uint8_t>(value);
    buffer[1] = static_cast<uint8_t>(value >> 8);
    buffer[2] = static_cast<uint8_t>(value >> 16);
    buffer[3] = static_cast<uint8_t>(value >> 24);
}

static inline uint32_t DecodeFixed32(const char* ptr) {
    const uint8_t* buffer = reinterpret_cast<const uint8_t*>(ptr);
    return (static_cast<uint32_t>(buffer[0])) |
           (static_cast<uint32_t>(buffer[1]) << 8) |
           (static_cast<uint32_t>(buffer[2]) << 16) |
           (static_cast<uint32_t>(buffer[3]) << 24);
}

} // namespace

BloomFilterPolicy::BloomFilterPolicy(int bits_per_key)
    : bits_per_key_(bits_per_key) {
    // Optimal probes = bits_per_key * ln(2)
    k_ = static_cast<size_t>(bits_per_key * 0.69314718056);
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
}

uint32_t BloomFilterPolicy::BloomHash(const Slice& key) {
    const uint32_t seed = 0xbc9f1d34;
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    size_t len = key.size();
    const char* data = key.data();

    uint32_t h = seed ^ static_cast<uint32_t>(len);

    while (len >= 4) {
        uint32_t k;
        std::memcpy(&k, data, sizeof(uint32_t));
        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    switch (len) {
        case 3: h ^= static_cast<uint8_t>(data[2]) << 16; [[fallthrough]];
        case 2: h ^= static_cast<uint8_t>(data[1]) << 8;  [[fallthrough]];
        case 1: h ^= static_cast<uint8_t>(data[0]);
                h *= m;
    };

    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return h;
}

void BloomFilterPolicy::CreateFilter(const Slice* keys, int n, std::string* dst) const {
    size_t bits = static_cast<size_t>(n * bits_per_key_);
    if (bits < 64) bits = 64;

    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    dst->push_back(static_cast<char>(k_)); // Store probe count k at end
    char* array = &(*dst)[init_size];

    for (int i = 0; i < n; ++i) {
        uint32_t h = BloomHash(keys[i]);
        const uint32_t delta = (h >> 17) | (h << 15);
        for (size_t j = 0; j < k_; ++j) {
            const uint32_t bitpos = h % bits;
            array[bitpos / 8] |= (1 << (bitpos % 8));
            h += delta;
        }
    }
}

bool BloomFilterPolicy::KeyMayMatch(const Slice& key, const Slice& bloom_filter) const {
    const size_t len = bloom_filter.size();
    if (len < 2) return true;

    const char* array = bloom_filter.data();
    const size_t bits = (len - 1) * 8;

    const size_t k = static_cast<uint8_t>(array[len - 1]);
    if (k > 30) {
        return true;
    }

    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (size_t j = 0; j < k; ++j) {
        const uint32_t bitpos = h % bits;
        if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) {
            return false;
        }
        h += delta;
    }
    return true;
}

FilterBlockBuilder::FilterBlockBuilder(const BloomFilterPolicy* policy, size_t block_size)
    : policy_(policy) {
    // Compute base_lg_ such that 1 << base_lg_ == block_size
    base_lg_ = 11;
    while ((1ULL << base_lg_) < block_size) {
        base_lg_++;
    }
}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
    uint64_t filter_index = (block_offset >> base_lg_);
    while (filter_index > filter_offsets_.size()) {
        GenerateFilter();
    }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
    start_.push_back(keys_data_.size());
    keys_data_.append(key.data(), key.size());
}

void FilterBlockBuilder::GenerateFilter() {
    const size_t num_keys = start_.size();
    if (num_keys == 0) {
        // Fast path for empty filter
        filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
        return;
    }

    start_.push_back(keys_data_.size());
    tmp_keys_.resize(num_keys);
    for (size_t i = 0; i < num_keys; ++i) {
        const char* base = keys_data_.data() + start_[i];
        size_t length = start_[i + 1] - start_[i];
        tmp_keys_[i] = Slice(base, length);
    }

    filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
    policy_->CreateFilter(tmp_keys_.data(), static_cast<int>(num_keys), &result_);

    tmp_keys_.clear();
    keys_data_.clear();
    start_.clear();
}

Slice FilterBlockBuilder::Finish() {
    if (!start_.empty()) {
        GenerateFilter();
    }

    // Append array of filter offsets
    const uint32_t array_offset = static_cast<uint32_t>(result_.size());
    for (size_t i = 0; i < filter_offsets_.size(); ++i) {
        char buf[4];
        EncodeFixed32(buf, filter_offsets_[i]);
        result_.append(buf, 4);
    }

    char buf[4];
    EncodeFixed32(buf, array_offset);
    result_.append(buf, 4);
    result_.push_back(static_cast<char>(base_lg_));

    return Slice(result_);
}

FilterBlockReader::FilterBlockReader(const BloomFilterPolicy* policy, const Slice& contents)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0) {
    const size_t n = contents.size();
    if (n < 5) return; // 1 byte base_lg + 4 bytes array_offset

    base_lg_ = static_cast<uint8_t>(contents[n - 1]);
    uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
    if (last_word > n - 5) return;

    data_ = contents.data();
    offset_ = data_ + last_word;
    num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) const {
    uint64_t index = block_offset >> base_lg_;
    if (index < num_) {
        uint32_t start = DecodeFixed32(offset_ + index * 4);
        uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
        if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
            Slice filter = Slice(data_ + start, limit - start);
            return policy_->KeyMayMatch(key, filter);
        } else if (start == limit) {
            return false; // Empty filter block
        }
    }
    return true; // Conservative fallback
}

} // namespace lsm
