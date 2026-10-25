#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include "slice.hpp"
#include "bloom_filter.hpp"

namespace lsm {

struct BloomShortCircuitStats {
    uint64_t total_probes{0};
    uint64_t useful_skips{0};    // Negative probes that avoided disk reads
    uint64_t positive_matches{0}; // Filter returned true
    double skip_ratio{0.0};

    std::string ToString() const;
};

// -----------------------------------------------------------------------------
// BloomShortCircuit: Fast Sub-Microsecond Bloom Filter Probe & Negative Skip Engine
// -----------------------------------------------------------------------------
class BloomShortCircuit {
public:
    BloomShortCircuit() = default;

    // Checks if the given user key may exist in the filter data.
    // If it returns false, the key definitely does NOT exist in the SSTable.
    bool KeyMayMatch(
        const FilterBlockReader* filter_reader,
        uint64_t block_offset,
        const Slice& user_key
    ) {
        total_probes_.fetch_add(1, std::memory_order_relaxed);
        if (!filter_reader) {
            positive_matches_.fetch_add(1, std::memory_order_relaxed);
            return true; // No filter present -> must probe
        }

        bool match = filter_reader->KeyMayMatch(block_offset, user_key);
        if (!match) {
            useful_skips_.fetch_add(1, std::memory_order_relaxed);
        } else {
            positive_matches_.fetch_add(1, std::memory_order_relaxed);
        }
        return match;
    }

    BloomShortCircuitStats GetStats() const {
        BloomShortCircuitStats stats;
        stats.total_probes = total_probes_.load(std::memory_order_relaxed);
        stats.useful_skips = useful_skips_.load(std::memory_order_relaxed);
        stats.positive_matches = positive_matches_.load(std::memory_order_relaxed);
        if (stats.total_probes > 0) {
            stats.skip_ratio = static_cast<double>(stats.useful_skips) / static_cast<double>(stats.total_probes);
        }
        return stats;
    }

    void Reset() {
        total_probes_.store(0, std::memory_order_relaxed);
        useful_skips_.store(0, std::memory_order_relaxed);
        positive_matches_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> total_probes_{0};
    std::atomic<uint64_t> useful_skips_{0};
    std::atomic<uint64_t> positive_matches_{0};
};

inline std::string BloomShortCircuitStats::ToString() const {
    return "Bloom Filter Stats: Probes=" + std::to_string(total_probes) +
           ", Useful Skips=" + std::to_string(useful_skips) +
           ", Positives=" + std::to_string(positive_matches) +
           ", Skip Ratio=" + std::to_string(skip_ratio * 100.0) + "%";
}

} // namespace lsm
