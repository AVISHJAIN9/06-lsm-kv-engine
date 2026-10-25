#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "slice.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// BloomFilterPolicy: Mathematical optimal bit-per-key Bloom filter generator
// False positive probability <= 1% at 10 bits/key with double hashing
// -----------------------------------------------------------------------------
class BloomFilterPolicy {
public:
    explicit BloomFilterPolicy(int bits_per_key = 10);
    ~BloomFilterPolicy() = default;

    const char* Name() const { return "lsm.BuiltinBloomFilter2"; }

    // Generates a Bloom filter bitset from an array of keys and appends to *dst
    void CreateFilter(const Slice* keys, int n, std::string* dst) const;

    // Evaluates key membership in the filter bitset
    bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const;

    int bits_per_key() const { return bits_per_key_; }
    size_t num_probes() const { return k_; }

    static uint32_t BloomHash(const Slice& key);

private:
    int bits_per_key_{10};
    size_t k_{7}; // Probes = bits_per_key * ln(2)
};

// -----------------------------------------------------------------------------
// FilterBlockBuilder: Generates block-based localized filters per 2KB data block
// -----------------------------------------------------------------------------
class FilterBlockBuilder {
public:
    explicit FilterBlockBuilder(const BloomFilterPolicy* policy, size_t block_size = 2048);

    void StartBlock(uint64_t block_offset);
    void AddKey(const Slice& key);
    Slice Finish();

private:
    void GenerateFilter();

    const BloomFilterPolicy* policy_{nullptr};
    std::string keys_data_;
    std::vector<size_t> start_;
    std::string result_;
    std::vector<Slice> tmp_keys_;
    std::vector<uint32_t> filter_offsets_;
    size_t base_lg_{11}; // 2^11 = 2048 bytes per filter slice
};

// -----------------------------------------------------------------------------
// FilterBlockReader: Localized block filter evaluator
// -----------------------------------------------------------------------------
class FilterBlockReader {
public:
    FilterBlockReader(const BloomFilterPolicy* policy, const Slice& contents);

    bool KeyMayMatch(uint64_t block_offset, const Slice& key) const;

private:
    const BloomFilterPolicy* policy_{nullptr};
    const char* data_{nullptr};
    const char* offset_{nullptr};
    size_t num_{0};
    size_t base_lg_{11};
};

} // namespace lsm
