#include "write_stall_controller.hpp"
#include <algorithm>

namespace lsm {

WriteStallController::WriteStallController(
    size_t imm_slowdown_trigger,
    size_t imm_stop_trigger,
    size_t l0_slowdown_trigger,
    size_t l0_stop_trigger
) : imm_slowdown_trigger_(imm_slowdown_trigger),
    imm_stop_trigger_(imm_stop_trigger),
    l0_slowdown_trigger_(l0_slowdown_trigger),
    l0_stop_trigger_(l0_stop_trigger) {}

WriteStallState WriteStallController::Evaluate(size_t num_imm, size_t num_l0) const {
    if (num_imm >= imm_stop_trigger_ || num_l0 >= l0_stop_trigger_) {
        return WriteStallState::kStopped;
    }
    if (num_imm >= imm_slowdown_trigger_ || num_l0 >= l0_slowdown_trigger_) {
        return WriteStallState::kDelayed;
    }
    return WriteStallState::kNormal;
}

uint64_t WriteStallController::CalculateDelayMicros(size_t num_imm, size_t num_l0) const {
    size_t imm_excess = (num_imm >= imm_slowdown_trigger_) ? (num_imm - imm_slowdown_trigger_ + 1) : 0;
    size_t l0_excess = (num_l0 >= l0_slowdown_trigger_) ? (num_l0 - l0_slowdown_trigger_ + 1) : 0;
    size_t total_excess = std::max(imm_excess, l0_excess);

    if (total_excess == 0) return 0;

    // Exponential backoff scaling
    uint64_t delay = base_delay_micros_ * (1ULL << std::min<size_t>(total_excess - 1, 5));
    return std::min(delay, max_delay_micros_);
}

WriteStallState WriteStallController::ApplyBackpressure(
    size_t num_imm,
    size_t num_l0,
    std::condition_variable& cv_stall,
    std::unique_lock<std::mutex>& lock
) {
    WriteStallState state = Evaluate(num_imm, num_l0);

    if (state == WriteStallState::kNormal) {
        return state;
    }

    if (state == WriteStallState::kDelayed) {
        uint64_t delay_us = CalculateDelayMicros(num_imm, num_l0);
        {
            std::lock_guard<std::mutex> stat_lock(mu_);
            stats_.total_delayed_writes++;
            stats_.total_stall_duration_micros += delay_us;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
        return state;
    }

    if (state == WriteStallState::kStopped) {
        auto t0 = std::chrono::high_resolution_clock::now();
        // Block on CV until a flush completion signal arrives or timeout after 5ms
        cv_stall.wait_for(lock, std::chrono::milliseconds(5), [&] {
            return Evaluate(num_imm, num_l0) != WriteStallState::kStopped;
        });
        auto t1 = std::chrono::high_resolution_clock::now();
        uint64_t dur_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        {
            std::lock_guard<std::mutex> stat_lock(mu_);
            stats_.total_stopped_writes++;
            stats_.total_stall_duration_micros += dur_us;
        }
        return state;
    }

    return state;
}

void WriteStallController::SetTriggers(
    size_t imm_slowdown,
    size_t imm_stop,
    size_t l0_slowdown,
    size_t l0_stop
) {
    imm_slowdown_trigger_ = imm_slowdown;
    imm_stop_trigger_ = imm_stop;
    l0_slowdown_trigger_ = l0_slowdown;
    l0_stop_trigger_ = l0_stop;
}

WriteStallStats WriteStallController::GetStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

void WriteStallController::ResetStats() {
    std::lock_guard<std::mutex> lock(mu_);
    stats_ = WriteStallStats{};
}

std::string WriteStallController::StateToString(WriteStallState state) const {
    switch (state) {
        case WriteStallState::kNormal: return "NORMAL";
        case WriteStallState::kDelayed: return "DELAYED";
        case WriteStallState::kStopped: return "STOPPED";
    }
    return "UNKNOWN";
}

} // namespace lsm
