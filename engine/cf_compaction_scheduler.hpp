#pragma once

#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <cstdint>
#include "column_family.hpp"
#include "leveled_compaction.hpp"
#include "size_tiered_compaction.hpp"
#include "compaction_picker.hpp"

namespace lsm {

enum class CFCompactionStyle {
    kLeveled,
    kSizeTiered
};

struct CFCompactionCandidate {
    uint32_t cf_id{0};
    std::string cf_name{};
    CFCompactionStyle style{CFCompactionStyle::kLeveled};
    int level{0};
    int output_level{1};
    double priority_score{0.0};
    std::vector<FileMetaData> inputs{};
    std::vector<FileMetaData> output_level_inputs{};
    uint64_t total_input_bytes{0};
};

// CFCompactionScheduler: Independent priority-driven compaction score evaluator
// across heterogeneous multi-tenant column families (Feature 103).
class CFCompactionScheduler {
public:
    CFCompactionScheduler();
    ~CFCompactionScheduler() = default;

    CFCompactionScheduler(const CFCompactionScheduler&) = delete;
    CFCompactionScheduler& operator=(const CFCompactionScheduler&) = delete;

    // Registers a specific column family's compaction policy
    void RegisterColumnFamily(
        uint32_t cf_id,
        CFCompactionStyle style,
        const LeveledCompactionOptions& leveled_opt = {},
        const SizeTieredCompactionOptions& stcs_opt = {}
    );

    void UnregisterColumnFamily(uint32_t cf_id);

    // Evaluates all live column families, computes independent level scores,
    // and picks the highest-priority cross-CF compaction job (latency <= 10 us/CF).
    std::optional<CFCompactionCandidate> PickBestCompaction(
        ColumnFamilySet* cf_set,
        const std::unordered_set<uint64_t>& files_being_compacted = {}
    );

    // Per-CF score evaluation
    std::vector<CompactionLevelScore> EvaluateCFScores(
        uint32_t cf_id,
        const ColumnFamilyData* cfd
    ) const;

private:
    struct CFPolicy {
        uint32_t cf_id{0};
        CFCompactionStyle style{CFCompactionStyle::kLeveled};
        LeveledCompactionOptions leveled_opt{};
        SizeTieredCompactionOptions stcs_opt{};
        std::unique_ptr<CompactionPicker> leveled_picker{nullptr};
    };

    mutable std::mutex mutex_{};
    std::unordered_map<uint32_t, CFPolicy> policies_{};
};

} // namespace lsm
