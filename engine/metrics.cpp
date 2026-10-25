#include "metrics.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace lsm {

LatencyHistogram::LatencyHistogram() {
    Reset();
}

void LatencyHistogram::Reset() {
    for (auto& b : buckets_) {
        b.store(0, std::memory_order_relaxed);
    }
    count_.store(0, std::memory_order_relaxed);
    sum_ns_.store(0, std::memory_order_relaxed);
    min_ns_.store(UINT64_MAX, std::memory_order_relaxed);
    max_ns_.store(0, std::memory_order_relaxed);
}

size_t LatencyHistogram::NanosToBucket(uint64_t nanos) {
    if (nanos == 0) return 0;
    // Logarithmic bucketing using leading zeros
    // 64-bit leading zeros gives index 0..63
    #if defined(__GNUC__) || defined(__clang__)
    int lz = __builtin_clzll(nanos);
    size_t idx = static_cast<size_t>(63 - lz);
    #else
    size_t idx = 0;
    uint64_t temp = nanos;
    while (temp >>= 1) {
        idx++;
    }
    #endif
    return std::min(idx, kNumBuckets - 1);
}

uint64_t LatencyHistogram::BucketToNanos(size_t bucket) {
    if (bucket == 0) return 0;
    return (1ULL << bucket);
}

void LatencyHistogram::Record(uint64_t nanos) {
    size_t b = NanosToBucket(nanos);
    buckets_[b].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    sum_ns_.fetch_add(nanos, std::memory_order_relaxed);

    uint64_t curr_min = min_ns_.load(std::memory_order_relaxed);
    while (nanos < curr_min && !min_ns_.compare_exchange_weak(curr_min, nanos, std::memory_order_relaxed)) {}

    uint64_t curr_max = max_ns_.load(std::memory_order_relaxed);
    while (nanos > curr_max && !max_ns_.compare_exchange_weak(curr_max, nanos, std::memory_order_relaxed)) {}
}

LatencyStats LatencyHistogram::GetStats() const {
    LatencyStats stats;
    stats.count = count_.load(std::memory_order_relaxed);
    if (stats.count == 0) {
        return stats;
    }

    stats.min_ns = min_ns_.load(std::memory_order_relaxed);
    stats.max_ns = max_ns_.load(std::memory_order_relaxed);
    uint64_t sum = sum_ns_.load(std::memory_order_relaxed);
    stats.mean_ns = static_cast<double>(sum) / stats.count;

    uint64_t target_p50 = static_cast<uint64_t>(std::ceil(stats.count * 0.50));
    uint64_t target_p90 = static_cast<uint64_t>(std::ceil(stats.count * 0.90));
    uint64_t target_p99 = static_cast<uint64_t>(std::ceil(stats.count * 0.99));
    uint64_t target_p999 = static_cast<uint64_t>(std::ceil(stats.count * 0.999));

    uint64_t cumulative = 0;
    bool found_p50 = false, found_p90 = false, found_p99 = false, found_p999 = false;

    for (size_t i = 0; i < kNumBuckets; ++i) {
        cumulative += buckets_[i].load(std::memory_order_relaxed);
        if (!found_p50 && cumulative >= target_p50) {
            stats.p50_ns = BucketToNanos(i);
            found_p50 = true;
        }
        if (!found_p90 && cumulative >= target_p90) {
            stats.p90_ns = BucketToNanos(i);
            found_p90 = true;
        }
        if (!found_p99 && cumulative >= target_p99) {
            stats.p99_ns = BucketToNanos(i);
            found_p99 = true;
        }
        if (!found_p999 && cumulative >= target_p999) {
            stats.p999_ns = BucketToNanos(i);
            found_p999 = true;
        }
    }

    return stats;
}

MetricsCollector& MetricsCollector::Instance() {
    static MetricsCollector instance;
    return instance;
}

MetricsCollector::MetricsCollector() {
    Reset();
}

void MetricsCollector::Reset() {
    for (auto& h : histograms_) {
        h.Reset();
    }
    for (auto& c : counters_) {
        c.store(0, std::memory_order_relaxed);
    }
}

void MetricsCollector::RecordLatency(MetricType type, uint64_t nanos) {
    size_t idx = static_cast<size_t>(type);
    if (idx < histograms_.size()) {
        histograms_[idx].Record(nanos);
    }
}

void MetricsCollector::AddCounter(CounterType type, uint64_t delta) {
    size_t idx = static_cast<size_t>(type);
    if (idx < counters_.size()) {
        counters_[idx].fetch_add(delta, std::memory_order_relaxed);
    }
}

uint64_t MetricsCollector::GetCounter(CounterType type) const {
    size_t idx = static_cast<size_t>(type);
    if (idx < counters_.size()) {
        return counters_[idx].load(std::memory_order_relaxed);
    }
    return 0;
}

LatencyStats MetricsCollector::GetLatencyStats(MetricType type) const {
    size_t idx = static_cast<size_t>(type);
    if (idx < histograms_.size()) {
        return histograms_[idx].GetStats();
    }
    return LatencyStats{};
}

MetricsSnapshot MetricsCollector::GetSnapshot() const {
    MetricsSnapshot snapshot;
    for (size_t i = 0; i < histograms_.size(); ++i) {
        snapshot.latencies[i] = histograms_[i].GetStats();
    }
    for (size_t i = 0; i < counters_.size(); ++i) {
        snapshot.counters[i] = counters_[i].load(std::memory_order_relaxed);
    }
    return snapshot;
}

static const char* MetricTypeName(MetricType t) {
    switch (t) {
        case MetricType::kReadLatency: return "read_latency";
        case MetricType::kWriteLatency: return "write_latency";
        case MetricType::kScanLatency: return "scan_latency";
        case MetricType::kWalSyncLatency: return "wal_sync_latency";
        case MetricType::kWalAppendLatency: return "wal_append_latency";
        case MetricType::kTxnCommitLatency: return "txn_commit_latency";
        default: return "unknown_latency";
    }
}

static const char* CounterTypeName(CounterType c) {
    switch (c) {
        case CounterType::kBytesWritten: return "bytes_written";
        case CounterType::kBytesRead: return "bytes_read";
        case CounterType::kOpsPut: return "ops_put";
        case CounterType::kOpsGet: return "ops_get";
        case CounterType::kOpsDelete: return "ops_delete";
        case CounterType::kOpsScan: return "ops_scan";
        case CounterType::kOpsBatchWrite: return "ops_batch_write";
        case CounterType::kWalSyncCount: return "wal_sync_count";
        case CounterType::kWalBytesFlushed: return "wal_bytes_flushed";
        case CounterType::kWalRotations: return "wal_rotations";
        case CounterType::kGroupCommitBatches: return "group_commit_batches";
        default: return "unknown_counter";
    }
}

std::string MetricsSnapshot::ToJson() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"counters\": {\n";
    for (size_t i = 0; i < counters.size(); ++i) {
        oss << "    \"" << CounterTypeName(static_cast<CounterType>(i)) << "\": " << counters[i];
        if (i + 1 < counters.size()) oss << ",";
        oss << "\n";
    }
    oss << "  },\n";
    oss << "  \"latencies\": {\n";
    for (size_t i = 0; i < latencies.size(); ++i) {
        const auto& s = latencies[i];
        oss << "    \"" << MetricTypeName(static_cast<MetricType>(i)) << "\": {\n";
        oss << "      \"count\": " << s.count << ",\n";
        oss << "      \"min_ns\": " << (s.count ? s.min_ns : 0) << ",\n";
        oss << "      \"max_ns\": " << s.max_ns << ",\n";
        oss << "      \"mean_ns\": " << std::fixed << std::setprecision(2) << s.mean_ns << ",\n";
        oss << "      \"p50_ns\": " << s.p50_ns << ",\n";
        oss << "      \"p90_ns\": " << s.p90_ns << ",\n";
        oss << "      \"p99_ns\": " << s.p99_ns << ",\n";
        oss << "      \"p999_ns\": " << s.p999_ns << "\n";
        oss << "    }";
        if (i + 1 < latencies.size()) oss << ",";
        oss << "\n";
    }
    oss << "  }\n";
    oss << "}";
    return oss.str();
}

std::string MetricsSnapshot::ToString() const {
    return ToJson();
}

} // namespace lsm
