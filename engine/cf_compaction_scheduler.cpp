#include "cf_compaction_scheduler.hpp"
#include <algorithm>

namespace lsm {

CFCompactionScheduler::CFCompactionScheduler() = default;

void CFCompactionScheduler::RegisterColumnFamily(
    uint32_t cf_id,
    CFCompactionStyle style,
    const LeveledCompactionOptions& leveled_opt,
    const SizeTieredCompactionOptions& stcs_opt
) {
    std::lock_guard<std::mutex> lock(mutex_);
    CFPolicy policy;
    policy.cf_id = cf_id;
    policy.style = style;
    policy.leveled_opt = leveled_opt;
    policy.stcs_opt = stcs_opt;
    if (style == CFCompactionStyle::kLeveled) {
        policy.leveled_picker = std::make_unique<CompactionPicker>(leveled_opt);
    }
    policies_[cf_id] = std::move(policy);
}

void CFCompactionScheduler::UnregisterColumnFamily(uint32_t cf_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    policies_.erase(cf_id);
}

std::vector<CompactionLevelScore> CFCompactionScheduler::EvaluateCFScores(
    uint32_t cf_id,
    const ColumnFamilyData* cfd
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = policies_.find(cf_id);
    if (it == policies_.end() || !cfd) {
        return {};
    }

    const auto& policy = it->second;
    if (policy.style == CFCompactionStyle::kLeveled && policy.leveled_picker) {
        std::vector<std::vector<FileMetaData>> levels(Version::kNumLevels);
        for (int l = 0; l < Version::kNumLevels; ++l) {
            levels[l] = cfd->GetLevelFiles(l);
        }
        return policy.leveled_picker->ComputeLevelScores(levels);
    }

    // Size Tiered scoring: ratio of file count to min_threshold
    std::vector<CompactionLevelScore> scores;
    const auto& l0_files = cfd->GetLevelFiles(0);
    CompactionLevelScore s;
    s.level = 0;
    s.bytes = 0;
    for (const auto& f : l0_files) {
        s.bytes += f.file_size;
    }
    s.target_bytes = policy.stcs_opt.min_threshold * policy.stcs_opt.min_sstable_size;
    s.score = static_cast<double>(l0_files.size()) / static_cast<double>(policy.stcs_opt.min_threshold);
    scores.push_back(s);
    return scores;
}

std::optional<CFCompactionCandidate> CFCompactionScheduler::PickBestCompaction(
    ColumnFamilySet* cf_set,
    const std::unordered_set<uint64_t>& files_being_compacted
) {
    if (!cf_set) return std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);
    auto cfds = cf_set->GetAllCFDs();

    std::optional<CFCompactionCandidate> best_candidate = std::nullopt;
    double highest_score = 0.0;

    for (const auto& cfd : cfds) {
        if (!cfd || cfd->IsDropped()) continue;

        uint32_t cf_id = cfd->GetID();
        auto it = policies_.find(cf_id);
        if (it == policies_.end()) continue;

        const auto& policy = it->second;

        if (policy.style == CFCompactionStyle::kLeveled && policy.leveled_picker) {
            std::vector<std::vector<FileMetaData>> levels(Version::kNumLevels);
            for (int l = 0; l < Version::kNumLevels; ++l) {
                levels[l] = cfd->GetLevelFiles(l);
            }

            LeveledCompactionJob job;
            if (policy.leveled_picker->PickCompaction(levels, files_being_compacted, &job)) {
                if (job.score > highest_score) {
                    highest_score = job.score;
                    CFCompactionCandidate cand;
                    cand.cf_id = cf_id;
                    cand.cf_name = cfd->GetName();
                    cand.style = CFCompactionStyle::kLeveled;
                    cand.level = job.level;
                    cand.output_level = job.level + 1;
                    cand.priority_score = job.score;
                    cand.inputs = job.inputs_level;
                    cand.output_level_inputs = job.inputs_next_level;
                    cand.total_input_bytes = 0;
                    for (const auto& f : job.inputs_level) {
                        cand.total_input_bytes += f.file_size;
                    }
                    for (const auto& f : job.inputs_next_level) {
                        cand.total_input_bytes += f.file_size;
                    }
                    best_candidate = cand;
                }
            }
        } else if (policy.style == CFCompactionStyle::kSizeTiered) {
            SizeTieredCompactor stcs(policy.stcs_opt);
            const auto& all_files = cfd->GetLevelFiles(0);
            SizeTieredCompactionJob job;
            if (stcs.PickCompaction(all_files, &job)) {
                double score = static_cast<double>(job.input_files.size()) /
                               static_cast<double>(policy.stcs_opt.min_threshold);
                if (score > highest_score) {
                    highest_score = score;
                    CFCompactionCandidate cand;
                    cand.cf_id = cf_id;
                    cand.cf_name = cfd->GetName();
                    cand.style = CFCompactionStyle::kSizeTiered;
                    cand.level = 0;
                    cand.output_level = 0;
                    cand.priority_score = score;
                    cand.inputs = job.input_files;
                    cand.total_input_bytes = 0;
                    for (const auto& f : job.input_files) {
                        cand.total_input_bytes += f.file_size;
                    }
                    best_candidate = cand;
                }
            }
        }
    }

    return best_candidate;
}

} // namespace lsm
