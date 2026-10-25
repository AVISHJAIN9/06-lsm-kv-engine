#pragma once

#include <string>
#include <cstdint>
#include <sstream>
#include "types.hpp"

namespace lsm {

enum class HealthStatus : uint8_t {
    HEALTHY = 0,
    DEGRADED = 1,
    CORRUPTED = 2
};

struct EngineHealthReport {
    HealthStatus status{HealthStatus::HEALTHY};
    size_t active_memtable_bytes{0};
    size_t active_snapshots{0};
    SequenceNumber current_sequence{0};
    size_t total_keys{0};
    std::string details{"System operates normally with all components healthy."};

    std::string to_json() const {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"status\": \"" << (status == HealthStatus::HEALTHY ? "HEALTHY" : (status == HealthStatus::DEGRADED ? "DEGRADED" : "CORRUPTED")) << "\",\n";
        ss << "  \"active_memtable_bytes\": " << active_memtable_bytes << ",\n";
        ss << "  \"active_snapshots\": " << active_snapshots << ",\n";
        ss << "  \"current_sequence\": " << current_sequence << ",\n";
        ss << "  \"total_keys\": " << total_keys << ",\n";
        ss << "  \"details\": \"" << details << "\"\n";
        ss << "}";
        return ss.str();
    }
};

} // namespace lsm
