#include "wal_metrics.hpp"
#include <cmath>
#include <algorithm>

namespace lsm {

WALLatencyHistogram::WALLatencyHistogram() {
    Reset();
}

void WALLatencyHistogram::Reset() {
    for (auto& bin : bins_) {
        bin.store(0, std::memory_order_relaxed);
    }
    max_duration_micros_.store(0, std::memory_order_relaxed);
    total_count_.store(0, std::memory_order_relaxed);
}

size_t WALLatencyHistogram::DurationToBin(uint64_t duration_micros) {
    if (duration_micros == 0) return 0;
    unsigned int clz = __builtin_clzll(duration_micros);
    size_t bin = (64 > clz) ? (63 - clz) : 0;
    if (bin >= kNumBins) bin = kNumBins - 1;
    return bin;
}

uint64_t WALLatencyHistogram::BinToDuration(size_t bin) {
    if (bin == 0) return 1;
    return (1ULL << bin);
}

void WALLatencyHistogram::Record(uint64_t duration_micros) {
    size_t bin = DurationToBin(duration_micros);
    bins_[bin].fetch_add(1, std::memory_order_relaxed);
    total_count_.fetch_add(1, std::memory_order_relaxed);

    uint64_t cur_max = max_duration_micros_.load(std::memory_order_relaxed);
    while (duration_micros > cur_max &&
           !max_duration_micros_.compare_exchange_weak(cur_max, duration_micros, std::memory_order_relaxed)) {
        // retry
    }
}

uint64_t WALLatencyHistogram::GetCount() const {
    return total_count_.load(std::memory_order_relaxed);
}

uint64_t WALLatencyHistogram::GetMax() const {
    return max_duration_micros_.load(std::memory_order_relaxed);
}

uint64_t WALLatencyHistogram::GetPercentile(double percentile) const {
    uint64_t total = total_count_.load(std::memory_order_relaxed);
    if (total == 0) return 0;

    uint64_t target_count = static_cast<uint64_t>(std::ceil((percentile / 100.0) * total));
    uint64_t cumulative = 0;

    for (size_t i = 0; i < kNumBins; ++i) {
        cumulative += bins_[i].load(std::memory_order_relaxed);
        if (cumulative >= target_count) {
            return BinToDuration(i);
        }
    }
    return GetMax();
}

std::string WALMetricsSnapshot::ToJSON() const {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"wal_write_bytes_total\": " << wal_write_bytes_total << ",\n"
       << "  \"wal_sync_count_total\": " << wal_sync_count_total << ",\n"
       << "  \"wal_sync_micros_total\": " << wal_sync_micros_total << ",\n"
       << "  \"wal_append_count_total\": " << wal_append_count_total << ",\n"
       << "  \"wal_append_micros_total\": " << wal_append_micros_total << ",\n"
       << "  \"wal_fragmentation_bytes_total\": " << wal_fragmentation_bytes_total << ",\n"
       << "  \"wal_active_file_number\": " << wal_active_file_number << ",\n"
       << "  \"sync_latency_us\": {\n"
       << "    \"p50\": " << sync_latency_p50_us << ",\n"
       << "    \"p95\": " << sync_latency_p95_us << ",\n"
       << "    \"p99\": " << sync_latency_p99_us << ",\n"
       << "    \"max\": " << sync_latency_max_us << "\n"
       << "  },\n"
       << "  \"append_latency_us\": {\n"
       << "    \"p50\": " << append_latency_p50_us << ",\n"
       << "    \"p95\": " << append_latency_p95_us << ",\n"
       << "    \"p99\": " << append_latency_p99_us << ",\n"
       << "    \"max\": " << append_latency_max_us << "\n"
       << "  }\n"
       << "}\n";
    return ss.str();
}

WALMetrics::WALMetrics() {
    Reset();
}

void WALMetrics::Reset() {
    write_bytes_.store(0, std::memory_order_relaxed);
    sync_count_.store(0, std::memory_order_relaxed);
    sync_micros_.store(0, std::memory_order_relaxed);
    append_count_.store(0, std::memory_order_relaxed);
    append_micros_.store(0, std::memory_order_relaxed);
    fragmentation_bytes_.store(0, std::memory_order_relaxed);
    active_file_number_.store(0, std::memory_order_relaxed);
    sync_histogram_.Reset();
    append_histogram_.Reset();
}

void WALMetrics::RecordAppend(size_t bytes, uint64_t duration_micros) {
    write_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    append_count_.fetch_add(1, std::memory_order_relaxed);
    append_micros_.fetch_add(duration_micros, std::memory_order_relaxed);
    append_histogram_.Record(duration_micros);
}

void WALMetrics::RecordSync(uint64_t duration_micros) {
    sync_count_.fetch_add(1, std::memory_order_relaxed);
    sync_micros_.fetch_add(duration_micros, std::memory_order_relaxed);
    sync_histogram_.Record(duration_micros);
}

void WALMetrics::RecordFragmentation(size_t padding_bytes) {
    fragmentation_bytes_.fetch_add(padding_bytes, std::memory_order_relaxed);
}

void WALMetrics::SetActiveFileNumber(uint64_t file_num) {
    active_file_number_.store(file_num, std::memory_order_relaxed);
}

WALMetricsSnapshot WALMetrics::GetSnapshot() const {
    WALMetricsSnapshot s;
    s.wal_write_bytes_total = write_bytes_.load(std::memory_order_relaxed);
    s.wal_sync_count_total = sync_count_.load(std::memory_order_relaxed);
    s.wal_sync_micros_total = sync_micros_.load(std::memory_order_relaxed);
    s.wal_append_count_total = append_count_.load(std::memory_order_relaxed);
    s.wal_append_micros_total = append_micros_.load(std::memory_order_relaxed);
    s.wal_fragmentation_bytes_total = fragmentation_bytes_.load(std::memory_order_relaxed);
    s.wal_active_file_number = active_file_number_.load(std::memory_order_relaxed);

    s.sync_latency_p50_us = sync_histogram_.GetPercentile(50.0);
    s.sync_latency_p95_us = sync_histogram_.GetPercentile(95.0);
    s.sync_latency_p99_us = sync_histogram_.GetPercentile(99.0);
    s.sync_latency_max_us = sync_histogram_.GetMax();

    s.append_latency_p50_us = append_histogram_.GetPercentile(50.0);
    s.append_latency_p95_us = append_histogram_.GetPercentile(95.0);
    s.append_latency_p99_us = append_histogram_.GetPercentile(99.0);
    s.append_latency_max_us = append_histogram_.GetMax();

    return s;
}

} // namespace lsm
