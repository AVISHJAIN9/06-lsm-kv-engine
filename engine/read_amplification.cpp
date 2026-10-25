#include "read_amplification.hpp"
#include <sstream>
#include <iomanip>

namespace lsm {

std::string RAFSummary::ToString() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "Read Amplification Factor (RAF) Summary:\n"
        << "  - Logical Get Requests:     " << logical_get_requests << "\n"
        << "  - MemTable Fast-Path Hits:  " << memtable_hits << "\n"
        << "  - SSTable Files Probed:     " << sstable_files_probed << "\n"
        << "  - Physical Disk Block Reads:" << data_blocks_read_from_disk << "\n"
        << "  - Block Cache Hits:         " << data_blocks_read_from_cache << "\n"
        << "  - File RAF:                 " << file_raf << " files/get\n"
        << "  - Disk Block RAF:           " << block_raf << " blocks/get\n"
        << "  - Cache Hit Ratio:          " << (cache_hit_ratio * 100.0) << "%\n";
    return oss.str();
}

RAFSummary ReadAmplificationTracker::GetSummary() const {
    RAFSummary summary;
    summary.logical_get_requests = logical_get_requests_.load(std::memory_order_relaxed);
    summary.memtable_hits = memtable_hits_.load(std::memory_order_relaxed);
    summary.sstable_files_probed = sstable_files_probed_.load(std::memory_order_relaxed);
    summary.data_blocks_read_from_disk = data_blocks_read_from_disk_.load(std::memory_order_relaxed);
    summary.data_blocks_read_from_cache = data_blocks_read_from_cache_.load(std::memory_order_relaxed);
    summary.file_raf = GetFileRAF();
    summary.block_raf = GetBlockRAF();
    summary.cache_hit_ratio = GetCacheHitRatio();
    return summary;
}

} // namespace lsm
