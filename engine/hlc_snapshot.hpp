#pragma once

#include <cstdint>
#include <string>
#include <sstream>
#include <tuple>
#include <mutex>
#include <chrono>
#include "types.hpp"
#include "slice.hpp"

namespace lsm {

// HLCTimestamp: Hybrid Logical Clock timestamp (physical_ms, logical_counter)
struct HLCTimestamp {
    uint64_t physical_time_ms{0};
    uint64_t logical_counter{0};

    HLCTimestamp() = default;
    HLCTimestamp(uint64_t phys, uint64_t log)
        : physical_time_ms(phys), logical_counter(log) {}

    bool operator<(const HLCTimestamp& other) const {
        if (physical_time_ms != other.physical_time_ms) {
            return physical_time_ms < other.physical_time_ms;
        }
        return logical_counter < other.logical_counter;
    }

    bool operator<=(const HLCTimestamp& other) const {
        return !(other < *this);
    }

    bool operator>(const HLCTimestamp& other) const {
        return other < *this;
    }

    bool operator>=(const HLCTimestamp& other) const {
        return !(*this < other);
    }

    bool operator==(const HLCTimestamp& other) const {
        return physical_time_ms == other.physical_time_ms &&
               logical_counter == other.logical_counter;
    }

    bool operator!=(const HLCTimestamp& other) const {
        return !(*this == other);
    }

    std::string ToString() const {
        return std::to_string(physical_time_ms) + "." + std::to_string(logical_counter);
    }

    static HLCTimestamp FromString(const std::string& str) {
        size_t dot = str.find('.');
        if (dot == std::string::npos) {
            return HLCTimestamp(std::stoull(str), 0);
        }
        uint64_t phys = std::stoull(str.substr(0, dot));
        uint64_t log = std::stoull(str.substr(dot + 1));
        return HLCTimestamp(phys, log);
    }
};

// HybridLogicalClock: Thread-safe HLC generator advancing monotonic physical/logical times
class HybridLogicalClock {
public:
    HybridLogicalClock();
    ~HybridLogicalClock() = default;

    // Generates a new local monotonic HLC timestamp
    HLCTimestamp Now();

    // Updates clock with incoming message timestamp to maintain causal consistency
    HLCTimestamp Update(const HLCTimestamp& msg_ts);

    // Returns the latest issued timestamp without advancing
    HLCTimestamp Latest() const;

private:
    mutable std::mutex mutex_;
    HLCTimestamp latest_ts_{0, 0};

    static uint64_t GetPhysicalTimeMs();
};

// DistributedSnapshot: Binds a global HLC timestamp to provide cross-partition snapshot isolation
class DistributedSnapshot {
public:
    DistributedSnapshot() = default;
    explicit DistributedSnapshot(HLCTimestamp snapshot_ts)
        : snapshot_ts_(snapshot_ts) {}

    HLCTimestamp GetTimestamp() const { return snapshot_ts_; }

    // Visibility rule: record is visible if record_ts <= snapshot_ts
    bool IsVisible(const HLCTimestamp& record_ts) const {
        return record_ts <= snapshot_ts_;
    }

private:
    HLCTimestamp snapshot_ts_{0, 0};
};

} // namespace lsm
