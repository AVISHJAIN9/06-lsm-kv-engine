#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "status.hpp"
#include "leveled_compaction.hpp"

namespace lsm {

struct CompactionBenchmarkOptions {
    size_t num_keys{10000};
    size_t key_size{16};
    size_t value_size{128};
    double overwrite_percent{50.0};
    double tombstone_percent{20.0};
    std::string db_dir{"./test_compaction_bench"};
};

struct CompactionBenchmarkResult {
    uint64_t input_bytes{0};
    uint64_t output_bytes{0};
    size_t input_records{0};
    size_t output_records{0};
    size_t tombstones_purged{0};
    double elapsed_ms{0.0};
    double throughput_mb_sec{0.0};
    double records_per_sec{0.0};
    double space_reduction_pct{0.0};

    std::string ToString() const;
};

// -----------------------------------------------------------------------------
// CompactionBenchmark: Synthetic workload generator and compaction profiler
// -----------------------------------------------------------------------------
class CompactionBenchmark {
public:
    explicit CompactionBenchmark(CompactionBenchmarkOptions options);

    // Generates synthetic SSTable dataset, runs compaction, and profiles performance
    Status Run(CompactionBenchmarkResult* out_result);

private:
    CompactionBenchmarkOptions options_;
};

} // namespace lsm
