#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <memory>
#include "status.hpp"
#include "memtable_core.hpp"

namespace lsm {

struct ChaosStats {
    uint64_t total_ops{0};
    uint64_t total_puts{0};
    uint64_t total_deletes{0};
    uint64_t total_gets{0};
    uint64_t faults_injected{0};
    uint64_t recoveries_verified{0};
    bool consistency_verified{true};
};

// ChaosHarness: Multi-threaded stress, Jepsen-style fault injection, and crash-recovery validator (Feature 145)
class ChaosHarness {
public:
    explicit ChaosHarness(std::string test_dir);
    ~ChaosHarness() = default;

    ChaosHarness(const ChaosHarness&) = delete;
    ChaosHarness& operator=(const ChaosHarness&) = delete;

    // Runs high-rate concurrent stress test with simulated crash interruptions and I/O fault injections
    ChaosStats RunStressTest(
        size_t num_threads = 4,
        size_t ops_per_thread = 2500,
        double fault_rate = 0.05
    );

private:
    std::string test_dir_;
    mutable std::mutex ref_mutex_;
    std::unordered_map<std::string, std::string> reference_model_;
    std::atomic<uint64_t> sequence_{1};
};

} // namespace lsm
