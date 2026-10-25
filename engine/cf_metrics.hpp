#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <memory>
#include <cstdint>
#include <cstddef>

namespace lsm {

struct CFMetricsSnapshot {
    uint32_t cf_id{0};
    std::string cf_name{};
    uint64_t num_writes{0};
    uint64_t write_bytes{0};
    uint64_t num_reads{0};
    uint64_t read_bytes{0};
    uint64_t memtable_bytes{0};
    uint64_t sstable_files_count{0};
    uint64_t sstable_total_bytes{0};
    uint64_t compaction_runs{0};
    uint64_t compaction_input_bytes{0};
    uint64_t compaction_output_bytes{0};
    double write_amplification_factor{1.0};
    double read_amplification_factor{1.0};
};

struct GlobalMetricsSnapshot {
    size_t total_active_cfs{0};
    uint64_t total_writes{0};
    uint64_t total_write_bytes{0};
    uint64_t total_reads{0};
    uint64_t total_read_bytes{0};
    uint64_t total_sstable_files{0};
    uint64_t total_sstable_bytes{0};
    uint64_t total_compaction_input_bytes{0};
    uint64_t total_compaction_output_bytes{0};
    double global_waf{1.0};
};

// CFInternalMetrics: Atomic lock-free counter bank for a single column family (Feature 108).
class CFInternalMetrics {
public:
    explicit CFInternalMetrics(uint32_t cf_id, std::string name)
        : cf_id_(cf_id), name_(std::move(name)) {}

    void RecordWrite(size_t bytes) {
        num_writes_.fetch_add(1, std::memory_order_relaxed);
        write_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }

    void RecordRead(size_t bytes) {
        num_reads_.fetch_add(1, std::memory_order_relaxed);
        read_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }

    void RecordCompaction(size_t in_bytes, size_t out_bytes) {
        compaction_runs_.fetch_add(1, std::memory_order_relaxed);
        compaction_input_bytes_.fetch_add(in_bytes, std::memory_order_relaxed);
        compaction_output_bytes_.fetch_add(out_bytes, std::memory_order_relaxed);
    }

    void UpdateMemoryAndFiles(size_t mem_bytes, size_t file_count, size_t file_bytes) {
        memtable_bytes_.store(mem_bytes, std::memory_order_relaxed);
        sstable_files_count_.store(file_count, std::memory_order_relaxed);
        sstable_total_bytes_.store(file_bytes, std::memory_order_relaxed);
    }

    CFMetricsSnapshot Snapshot() const;

    uint32_t id() const { return cf_id_; }
    const std::string& name() const { return name_; }

private:
    uint32_t cf_id_{0};
    std::string name_{};

    std::atomic<uint64_t> num_writes_{0};
    std::atomic<uint64_t> write_bytes_{0};
    std::atomic<uint64_t> num_reads_{0};
    std::atomic<uint64_t> read_bytes_{0};
    std::atomic<uint64_t> memtable_bytes_{0};
    std::atomic<uint64_t> sstable_files_count_{0};
    std::atomic<uint64_t> sstable_total_bytes_{0};
    std::atomic<uint64_t> compaction_runs_{0};
    std::atomic<uint64_t> compaction_input_bytes_{0};
    std::atomic<uint64_t> compaction_output_bytes_{0};
};

// CFTelemetryAggregator: Multi-tenant metric registry and telemetry reporting aggregator (Feature 108).
class CFTelemetryAggregator {
public:
    CFTelemetryAggregator() = default;
    ~CFTelemetryAggregator() = default;

    CFTelemetryAggregator(const CFTelemetryAggregator&) = delete;
    CFTelemetryAggregator& operator=(const CFTelemetryAggregator&) = delete;

    void RegisterCF(uint32_t cf_id, const std::string& name);
    void UnregisterCF(uint32_t cf_id);

    void RecordWrite(uint32_t cf_id, size_t bytes);
    void RecordRead(uint32_t cf_id, size_t bytes);
    void RecordCompaction(uint32_t cf_id, size_t in_bytes, size_t out_bytes);
    void UpdateMemoryAndFiles(uint32_t cf_id, size_t mem_bytes, size_t file_count, size_t file_bytes);

    CFMetricsSnapshot GetCFMetrics(uint32_t cf_id) const;
    GlobalMetricsSnapshot GetGlobalMetrics() const;

    std::string FormatTelemetryReport() const;

private:
    mutable std::mutex mutex_{};
    std::unordered_map<uint32_t, std::shared_ptr<CFInternalMetrics>> metrics_map_{};
};

} // namespace lsm
