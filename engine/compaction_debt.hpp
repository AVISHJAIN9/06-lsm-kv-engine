#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "leveled_compaction.hpp"

namespace lsm {

struct CompactionDebtReport {
    uint64_t total_debt_bytes{0};
    std::vector<uint64_t> level_debt_bytes{};
    std::vector<uint64_t> level_actual_bytes{};
    std::vector<uint64_t> level_target_bytes{};
    bool danger_watermark_crossed{false};

    std::string ToString() const;
};

// -----------------------------------------------------------------------------
// CompactionDebtTracker: Evaluates outstanding compaction backlog across levels
// -----------------------------------------------------------------------------
class CompactionDebtTracker {
public:
    explicit CompactionDebtTracker(
        LeveledCompactionOptions options,
        uint64_t danger_threshold_bytes = 64 * 1024 * 1024 // 64 MB
    );

    // Calculates outstanding debt in bytes for each level and across the entire LSM tree
    CompactionDebtReport CalculateDebt(
        const std::vector<std::vector<FileMetaData>>& levels
    ) const;

    uint64_t TargetBytesForLevel(int level) const;

    void SetDangerThreshold(uint64_t threshold_bytes) {
        danger_threshold_bytes_ = threshold_bytes;
    }

    uint64_t GetDangerThreshold() const {
        return danger_threshold_bytes_;
    }

private:
    LeveledCompactionOptions options_;
    uint64_t danger_threshold_bytes_{64 * 1024 * 1024};
};

} // namespace lsm
