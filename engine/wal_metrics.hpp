#pragma once

#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace lsm {

struct WALMetricsSnapshot {
    uint64_t wal_write_bytes_total{0};
    uint64_t wal_sync_count_total{0};
    uint64_t wal_sync_micros_total{0};
    uint64_t wal_append_count_total{0};
    uint64_t wal_append_micros_total{0};
    uint64_t wal_fragmentation_bytes_total{0};
    uint64_t wal_active_file_number{0};

    // Latency histograms (microseconds)
    uint64_t sync_latency_p50_us{0};
    uint64_t sync_latency_p95_us{0};
    uint64_t sync_latency_p99_us{0};
    uint64_t sync_latency_max_us{0};

    uint64_t append_latency_p50_us{0};
    uint64_t append_latency_p95_us{0};
    uint64_t append_latency_p99_us{0};
    uint64_t append_latency_max_us{0};

    std::string ToJSON() const;
};

// High-resolution, low-overhead atomic latency histogram with logarithmic binning
class WALLatencyHistogram {
public:
    WALLatencyHistogram();
    ~WALLatencyHistogram() = default;

    void Record(uint64_t duration_micros);
    void Reset();

    uint64_t GetPercentile(double percentile) const;
    uint64_t GetMax() const;
    uint64_t GetCount() const;

private:
    static constexpr size_t kNumBins = 64;
    std::array<std::atomic<uint64_t>, kNumBins> bins_{};
    std::atomic<uint64_t> max_duration_micros_{0};
    std::atomic<uint64_t> total_count_{0};

    static size_t DurationToBin(uint64_t duration_micros);
    static uint64_t BinToDuration(size_t bin);
};

class WALMetrics {
public:
    WALMetrics();
    ~WALMetrics() = default;

    WALMetrics(const WALMetrics&) = delete;
    WALMetrics& operator=(const WALMetrics&) = delete;

    void RecordAppend(size_t bytes, uint64_t duration_micros);
    void RecordSync(uint64_t duration_micros);
    void RecordFragmentation(size_t padding_bytes);
    void SetActiveFileNumber(uint64_t file_num);

    WALMetricsSnapshot GetSnapshot() const;
    void Reset();

private:
    std::atomic<uint64_t> write_bytes_{0};
    std::atomic<uint64_t> sync_count_{0};
    std::atomic<uint64_t> sync_micros_{0};
    std::atomic<uint64_t> append_count_{0};
    std::atomic<uint64_t> append_micros_{0};
    std::atomic<uint64_t> fragmentation_bytes_{0};
    std::atomic<uint64_t> active_file_number_{0};

    WALLatencyHistogram sync_histogram_{};
    WALLatencyHistogram append_histogram_{};
};

} // namespace lsm
