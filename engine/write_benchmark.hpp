#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "memtable_queue.hpp"

namespace lsm {

struct BenchmarkOptions {
    size_t num_threads{1};
    size_t num_keys{10000};
    size_t batch_size{1};
    size_t key_size{16};
    size_t val_size{100};
    bool random_keys{false};
};

struct BenchmarkResult {
    double total_time_ms{0.0};
    double ops_per_sec{0.0};
    double mb_per_sec{0.0};
    uint64_t p50_latency_ns{0};
    uint64_t p90_latency_ns{0};
    uint64_t p99_latency_ns{0};
    uint64_t p999_latency_ns{0};

    std::string ToString() const;
};

// -----------------------------------------------------------------------------
// WriteBenchmarkRunner: High-precision write-path latency and throughput runner
// -----------------------------------------------------------------------------
class WriteBenchmarkRunner {
public:
    static BenchmarkResult Run(MemTableQueue* queue, const BenchmarkOptions& options);
};

} // namespace lsm
