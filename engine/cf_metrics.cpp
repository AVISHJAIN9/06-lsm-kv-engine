#include "cf_metrics.hpp"
#include <sstream>
#include <iomanip>

namespace lsm {

CFMetricsSnapshot CFInternalMetrics::Snapshot() const {
    CFMetricsSnapshot snap;
    snap.cf_id = cf_id_;
    snap.cf_name = name_;
    snap.num_writes = num_writes_.load(std::memory_order_relaxed);
    snap.write_bytes = write_bytes_.load(std::memory_order_relaxed);
    snap.num_reads = num_reads_.load(std::memory_order_relaxed);
    snap.read_bytes = read_bytes_.load(std::memory_order_relaxed);
    snap.memtable_bytes = memtable_bytes_.load(std::memory_order_relaxed);
    snap.sstable_files_count = sstable_files_count_.load(std::memory_order_relaxed);
    snap.sstable_total_bytes = sstable_total_bytes_.load(std::memory_order_relaxed);
    snap.compaction_runs = compaction_runs_.load(std::memory_order_relaxed);
    snap.compaction_input_bytes = compaction_input_bytes_.load(std::memory_order_relaxed);
    snap.compaction_output_bytes = compaction_output_bytes_.load(std::memory_order_relaxed);

    if (snap.write_bytes > 0) {
        snap.write_amplification_factor = static_cast<double>(snap.compaction_output_bytes + snap.write_bytes) /
                                          static_cast<double>(snap.write_bytes);
    } else {
        snap.write_amplification_factor = 1.0;
    }

    snap.read_amplification_factor = (snap.num_reads > 0) ? 1.0 : 1.0;
    return snap;
}

void CFTelemetryAggregator::RegisterCF(uint32_t cf_id, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_map_[cf_id] = std::make_shared<CFInternalMetrics>(cf_id, name);
}

void CFTelemetryAggregator::UnregisterCF(uint32_t cf_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_map_.erase(cf_id);
}

void CFTelemetryAggregator::RecordWrite(uint32_t cf_id, size_t bytes) {
    std::shared_ptr<CFInternalMetrics> m;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = metrics_map_.find(cf_id);
        if (it != metrics_map_.end()) {
            m = it->second;
        }
    }
    if (m) {
        m->RecordWrite(bytes);
    }
}

void CFTelemetryAggregator::RecordRead(uint32_t cf_id, size_t bytes) {
    std::shared_ptr<CFInternalMetrics> m;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = metrics_map_.find(cf_id);
        if (it != metrics_map_.end()) {
            m = it->second;
        }
    }
    if (m) {
        m->RecordRead(bytes);
    }
}

void CFTelemetryAggregator::RecordCompaction(uint32_t cf_id, size_t in_bytes, size_t out_bytes) {
    std::shared_ptr<CFInternalMetrics> m;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = metrics_map_.find(cf_id);
        if (it != metrics_map_.end()) {
            m = it->second;
        }
    }
    if (m) {
        m->RecordCompaction(in_bytes, out_bytes);
    }
}

void CFTelemetryAggregator::UpdateMemoryAndFiles(
    uint32_t cf_id,
    size_t mem_bytes,
    size_t file_count,
    size_t file_bytes
) {
    std::shared_ptr<CFInternalMetrics> m;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = metrics_map_.find(cf_id);
        if (it != metrics_map_.end()) {
            m = it->second;
        }
    }
    if (m) {
        m->UpdateMemoryAndFiles(mem_bytes, file_count, file_bytes);
    }
}

CFMetricsSnapshot CFTelemetryAggregator::GetCFMetrics(uint32_t cf_id) const {
    std::shared_ptr<CFInternalMetrics> m;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = metrics_map_.find(cf_id);
        if (it != metrics_map_.end()) {
            m = it->second;
        }
    }
    if (m) {
        return m->Snapshot();
    }
    CFMetricsSnapshot empty;
    empty.cf_id = cf_id;
    return empty;
}

GlobalMetricsSnapshot CFTelemetryAggregator::GetGlobalMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    GlobalMetricsSnapshot g;
    g.total_active_cfs = metrics_map_.size();

    for (const auto& [_, m] : metrics_map_) {
        auto s = m->Snapshot();
        g.total_writes += s.num_writes;
        g.total_write_bytes += s.write_bytes;
        g.total_reads += s.num_reads;
        g.total_read_bytes += s.read_bytes;
        g.total_sstable_files += s.sstable_files_count;
        g.total_sstable_bytes += s.sstable_total_bytes;
        g.total_compaction_input_bytes += s.compaction_input_bytes;
        g.total_compaction_output_bytes += s.compaction_output_bytes;
    }

    if (g.total_write_bytes > 0) {
        g.global_waf = static_cast<double>(g.total_compaction_output_bytes + g.total_write_bytes) /
                       static_cast<double>(g.total_write_bytes);
    } else {
        g.global_waf = 1.0;
    }

    return g;
}

std::string CFTelemetryAggregator::FormatTelemetryReport() const {
    std::ostringstream oss;
    auto g = GetGlobalMetrics();
    oss << "=== Multi-Tenant Telemetry Report ===\n";
    oss << "Active CFs: " << g.total_active_cfs
        << " | Writes: " << g.total_writes << " (" << g.total_write_bytes << " B)"
        << " | Reads: " << g.total_reads << " (" << g.total_read_bytes << " B)\n"
        << "SSTables: " << g.total_sstable_files << " (" << g.total_sstable_bytes << " B)"
        << " | Global WAF: " << std::fixed << std::setprecision(2) << g.global_waf << "\n";

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, m] : metrics_map_) {
        auto s = m->Snapshot();
        oss << "  - CF [" << id << "] '" << s.cf_name << "': "
            << "writes=" << s.num_writes << " (" << s.write_bytes << "B), "
            << "memtable=" << s.memtable_bytes << "B, "
            << "sst_files=" << s.sstable_files_count << ", "
            << "WAF=" << std::fixed << std::setprecision(2) << s.write_amplification_factor << "\n";
    }
    return oss.str();
}

} // namespace lsm
