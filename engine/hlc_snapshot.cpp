#include "hlc_snapshot.hpp"
#include <algorithm>

namespace lsm {

uint64_t HybridLogicalClock::GetPhysicalTimeMs() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
}

HybridLogicalClock::HybridLogicalClock() {
    latest_ts_ = HLCTimestamp(GetPhysicalTimeMs(), 0);
}

HLCTimestamp HybridLogicalClock::Now() {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t phys_now = GetPhysicalTimeMs();

    if (phys_now > latest_ts_.physical_time_ms) {
        latest_ts_.physical_time_ms = phys_now;
        latest_ts_.logical_counter = 0;
    } else {
        latest_ts_.logical_counter++;
    }

    return latest_ts_;
}

HLCTimestamp HybridLogicalClock::Update(const HLCTimestamp& msg_ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t phys_now = GetPhysicalTimeMs();

    uint64_t max_phys = std::max({phys_now, latest_ts_.physical_time_ms, msg_ts.physical_time_ms});

    if (max_phys == latest_ts_.physical_time_ms && max_phys == msg_ts.physical_time_ms) {
        latest_ts_.logical_counter = std::max(latest_ts_.logical_counter, msg_ts.logical_counter) + 1;
    } else if (max_phys == latest_ts_.physical_time_ms) {
        latest_ts_.logical_counter++;
    } else if (max_phys == msg_ts.physical_time_ms) {
        latest_ts_.logical_counter = msg_ts.logical_counter + 1;
    } else {
        latest_ts_.logical_counter = 0;
    }

    latest_ts_.physical_time_ms = max_phys;
    return latest_ts_;
}

HLCTimestamp HybridLogicalClock::Latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_ts_;
}

} // namespace lsm
