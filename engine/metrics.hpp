#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include "slice.hpp"

namespace lsm {

enum class MetricType : uint8_t {
    kReadLatency = 0,
    kWriteLatency = 1,
    kScanLatency = 2,
    kWalSyncLatency = 3,
    kWalAppendLatency = 4,
    kTxnCommitLatency = 5,
    kCount = 6
};

enum class CounterType : uint8_t {
    kBytesWritten = 0,
    kBytesRead = 1,
    kOpsPut = 2,
    kOpsGet = 3,
    kOpsDelete = 4,
    kOpsScan = 5,
    kOpsBatchWrite = 6,
    kWalSyncCount = 7,
    kWalBytesFlushed = 8,
    kWalRotations = 9,
    kGroupCommitBatches = 10,
    kCount = 11
};

struct LatencyStats {
    uint64_t count{0};
    uint64_t min_ns{0};
    uint64_t max_ns{0};
    double mean_ns{0.0};
    uint64_t p50_ns{0};
    uint64_t p90_ns{0};
    uint64_t p99_ns{0};
    uint64_t p999_ns{0};
};

struct MetricsSnapshot {
    std::array<LatencyStats, static_cast<size_t>(MetricType::kCount)> latencies{};
    std::array<uint64_t, static_cast<size_t>(CounterType::kCount)> counters{};

    std::string ToJson() const;
    std::string ToString() const;
};

// Fast log-scale latency histogram
class LatencyHistogram {
public:
    static constexpr size_t kNumBuckets = 64;

    LatencyHistogram();
    ~LatencyHistogram() = default;

    void Record(uint64_t nanos);
    LatencyStats GetStats() const;
    void Reset();

private:
    static size_t NanosToBucket(uint64_t nanos);
    static uint64_t BucketToNanos(size_t bucket);

    std::array<std::atomic<uint64_t>, kNumBuckets> buckets_{};
    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> sum_ns_{0};
    std::atomic<uint64_t> min_ns_{UINT64_MAX};
    std::atomic<uint64_t> max_ns_{0};
};

class MetricsCollector {
public:
    static MetricsCollector& Instance();

    MetricsCollector();
    ~MetricsCollector() = default;

    void RecordLatency(MetricType type, uint64_t nanos);
    void AddCounter(CounterType type, uint64_t delta = 1);
    uint64_t GetCounter(CounterType type) const;
    LatencyStats GetLatencyStats(MetricType type) const;

    MetricsSnapshot GetSnapshot() const;
    void Reset();

private:
    std::array<LatencyHistogram, static_cast<size_t>(MetricType::kCount)> histograms_{};
    std::array<std::atomic<uint64_t>, static_cast<size_t>(CounterType::kCount)> counters_{};
};

class ScopedTimer {
public:
    ScopedTimer(MetricsCollector* collector, MetricType type)
        : collector_(collector), type_(type), start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        if (collector_) {
            auto end = std::chrono::steady_clock::now();
            uint64_t nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
            collector_->RecordLatency(type_, nanos);
        }
    }

private:
    MetricsCollector* collector_{nullptr};
    MetricType type_{MetricType::kReadLatency};
    std::chrono::steady_clock::time_point start_{};
};

} // namespace lsm
