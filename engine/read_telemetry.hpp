#pragma once

#include <cstdint>
#include <string>
#include <sstream>
#include <iomanip>

namespace lsm {

// -----------------------------------------------------------------------------
// ReadTierStats: Fine-grained per-query & thread-local read telemetry counters
// -----------------------------------------------------------------------------
struct ReadTierStats {
    uint32_t memtable_hits{0};
    uint32_t memtable_misses{0};
    uint32_t files_probed{0};
    uint32_t bloom_hits{0};
    uint32_t bloom_misses{0}; // Useful negative skips
    uint32_t index_blocks_loaded{0};
    uint32_t data_blocks_loaded_disk{0};
    uint32_t data_blocks_loaded_cache{0};

    void Reset() {
        memtable_hits = 0;
        memtable_misses = 0;
        files_probed = 0;
        bloom_hits = 0;
        bloom_misses = 0;
        index_blocks_loaded = 0;
        data_blocks_loaded_disk = 0;
        data_blocks_loaded_cache = 0;
    }

    void Merge(const ReadTierStats& other) {
        memtable_hits += other.memtable_hits;
        memtable_misses += other.memtable_misses;
        files_probed += other.files_probed;
        bloom_hits += other.bloom_hits;
        bloom_misses += other.bloom_misses;
        index_blocks_loaded += other.index_blocks_loaded;
        data_blocks_loaded_disk += other.data_blocks_loaded_disk;
        data_blocks_loaded_cache += other.data_blocks_loaded_cache;
    }

    std::string ToString() const {
        std::ostringstream oss;
        oss << "Read Tier Telemetry Stats:\n"
            << "  - MemTable Hits:            " << memtable_hits << "\n"
            << "  - MemTable Misses:          " << memtable_misses << "\n"
            << "  - SSTable Files Probed:     " << files_probed << "\n"
            << "  - Bloom Filter Hits:        " << bloom_hits << "\n"
            << "  - Bloom Negative Bypasses:  " << bloom_misses << "\n"
            << "  - Index Blocks Loaded:      " << index_blocks_loaded << "\n"
            << "  - Data Blocks (Disk IO):    " << data_blocks_loaded_disk << "\n"
            << "  - Data Blocks (Cache Hit):  " << data_blocks_loaded_cache << "\n";
        return oss.str();
    }
};

} // namespace lsm
