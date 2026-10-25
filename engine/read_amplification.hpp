#pragma once

#include <cstdint>
#include <atomic>
#include <string>

namespace lsm {

struct RAFSummary {
    uint64_t logical_get_requests{0};
    uint64_t memtable_hits{0};
    uint64_t sstable_files_probed{0};
    uint64_t data_blocks_read_from_disk{0};
    uint64_t data_blocks_read_from_cache{0};
    double file_raf{0.0};
    double block_raf{0.0};
    double cache_hit_ratio{0.0};

    std::string ToString() const;
};

// -----------------------------------------------------------------------------
// ReadAmplificationTracker: Real-time atomic accounting of read amplification
// -----------------------------------------------------------------------------
class ReadAmplificationTracker {
public:
    ReadAmplificationTracker() = default;

    void RecordLogicalGet() {
        logical_get_requests_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordMemTableHit() {
        memtable_hits_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordSSTableProbe() {
        sstable_files_probed_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordDiskBlockRead() {
        data_blocks_read_from_disk_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordCacheBlockRead() {
        data_blocks_read_from_cache_.fetch_add(1, std::memory_order_relaxed);
    }

    double GetFileRAF() const {
        uint64_t gets = logical_get_requests_.load(std::memory_order_relaxed);
        if (gets == 0) return 0.0;
        uint64_t probes = sstable_files_probed_.load(std::memory_order_relaxed);
        return static_cast<double>(probes) / static_cast<double>(gets);
    }

    double GetBlockRAF() const {
        uint64_t gets = logical_get_requests_.load(std::memory_order_relaxed);
        if (gets == 0) return 0.0;
        uint64_t blocks = data_blocks_read_from_disk_.load(std::memory_order_relaxed);
        return static_cast<double>(blocks) / static_cast<double>(gets);
    }

    double GetCacheHitRatio() const {
        uint64_t disk = data_blocks_read_from_disk_.load(std::memory_order_relaxed);
        uint64_t cache = data_blocks_read_from_cache_.load(std::memory_order_relaxed);
        uint64_t total = disk + cache;
        if (total == 0) return 1.0;
        return static_cast<double>(cache) / static_cast<double>(total);
    }

    RAFSummary GetSummary() const;

    void Reset() {
        logical_get_requests_.store(0, std::memory_order_relaxed);
        memtable_hits_.store(0, std::memory_order_relaxed);
        sstable_files_probed_.store(0, std::memory_order_relaxed);
        data_blocks_read_from_disk_.store(0, std::memory_order_relaxed);
        data_blocks_read_from_cache_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> logical_get_requests_{0};
    std::atomic<uint64_t> memtable_hits_{0};
    std::atomic<uint64_t> sstable_files_probed_{0};
    std::atomic<uint64_t> data_blocks_read_from_disk_{0};
    std::atomic<uint64_t> data_blocks_read_from_cache_{0};
};

} // namespace lsm
