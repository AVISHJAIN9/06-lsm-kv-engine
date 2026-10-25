#pragma once

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <thread>
#include <string>

namespace lsm {

enum class WriteStallState : uint8_t {
    kNormal = 0,
    kDelayed = 1,
    kStopped = 2
};

struct WriteStallStats {
    uint64_t total_delayed_writes{0};
    uint64_t total_stopped_writes{0};
    uint64_t total_stall_duration_micros{0};
};

class WriteStallController {
public:
    explicit WriteStallController(
        size_t imm_slowdown_trigger = 2,
        size_t imm_stop_trigger = 4,
        size_t l0_slowdown_trigger = 4,
        size_t l0_stop_trigger = 8
    );
    ~WriteStallController() = default;

    WriteStallState Evaluate(size_t num_imm, size_t num_l0) const;
    uint64_t CalculateDelayMicros(size_t num_imm, size_t num_l0) const;

    // Applies sleep pacing or condition-variable blocking backpressure
    WriteStallState ApplyBackpressure(
        size_t num_imm,
        size_t num_l0,
        std::condition_variable& cv_stall,
        std::unique_lock<std::mutex>& lock
    );

    void SetTriggers(size_t imm_slowdown, size_t imm_stop, size_t l0_slowdown, size_t l0_stop);

    WriteStallStats GetStats() const;
    void ResetStats();

    std::string StateToString(WriteStallState state) const;

private:
    size_t imm_slowdown_trigger_{2};
    size_t imm_stop_trigger_{4};
    size_t l0_slowdown_trigger_{4};
    size_t l0_stop_trigger_{8};

    uint64_t base_delay_micros_{50};
    uint64_t max_delay_micros_{1000};

    mutable std::mutex mu_{};
    WriteStallStats stats_{};
};

} // namespace lsm
