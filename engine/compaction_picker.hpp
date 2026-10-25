#pragma once

#include <vector>
#include <unordered_set>
#include <memory>
#include <cstdint>
#include "leveled_compaction.hpp"
#include "compaction_input_selector.hpp"

namespace lsm {

struct CompactionLevelScore {
    int level{-1};
    double score{0.0};
    uint64_t bytes{0};
    uint64_t target_bytes{0};
};

// -----------------------------------------------------------------------------
// CompactionPicker: Evaluates normalized level scores and selects candidate jobs
// -----------------------------------------------------------------------------
class CompactionPicker {
public:
    explicit CompactionPicker(LeveledCompactionOptions options);

    // Computes normalized priority scores for all levels (O(L) <= 5 us)
    std::vector<CompactionLevelScore> ComputeLevelScores(
        const std::vector<std::vector<FileMetaData>>& levels
    ) const;

    // Evaluates levels and selects the highest-scoring disjoint compaction job
    bool PickCompaction(
        const std::vector<std::vector<FileMetaData>>& levels,
        const std::unordered_set<uint64_t>& files_being_compacted,
        LeveledCompactionJob* out_job
    ) const;

    uint64_t TargetBytesForLevel(int level) const;

private:
    LeveledCompactionOptions options_;
};

} // namespace lsm
