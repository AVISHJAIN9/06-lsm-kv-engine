#include "write_amplification.hpp"
#include <sstream>
#include <iomanip>

namespace lsm {

std::string WAFSummary::ToString() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "Write Amplification Factor (WAF) Summary:\n"
        << "  - User Ingested Data:       " << bytes_user_written << " bytes\n"
        << "  - WAL Appends:              " << bytes_wal_written << " bytes\n"
        << "  - MemTable Flushes:         " << bytes_flush_written << " bytes\n"
        << "  - Compaction Writes:        " << bytes_compaction_written << " bytes\n"
        << "  - Total Physical Storage:   " << total_physical_bytes_written << " bytes\n"
        << "  - Cumulative WAF:           " << cumulative_waf << "x\n"
        << "  - Compaction WAF:           " << compaction_waf << "x\n";
    return oss.str();
}

WAFSummary WriteAmplificationTracker::GetSummary() const {
    WAFSummary summary;
    summary.bytes_user_written = bytes_user_written_.load(std::memory_order_relaxed);
    summary.bytes_wal_written = bytes_wal_written_.load(std::memory_order_relaxed);
    summary.bytes_flush_written = bytes_flush_written_.load(std::memory_order_relaxed);
    summary.bytes_compaction_written = bytes_compaction_written_.load(std::memory_order_relaxed);
    summary.total_physical_bytes_written = summary.bytes_wal_written +
                                           summary.bytes_flush_written +
                                           summary.bytes_compaction_written;
    summary.cumulative_waf = GetCumulativeWAF();
    summary.compaction_waf = GetCompactionWAF();
    return summary;
}

} // namespace lsm
