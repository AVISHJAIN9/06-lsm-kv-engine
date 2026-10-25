#include "compaction_picker.hpp"
#include <cmath>
#include <algorithm>

namespace lsm {

CompactionPicker::CompactionPicker(LeveledCompactionOptions options)
    : options_(options) {}

uint64_t CompactionPicker::TargetBytesForLevel(int level) const {
    if (level <= 0) {
        return options_.l0_file_num_compaction_trigger * options_.target_file_size;
    }
    double multiplier = std::pow(options_.level_multiplier, level - 1);
    return static_cast<uint64_t>(static_cast<double>(options_.base_level_bytes) * multiplier);
}

std::vector<CompactionLevelScore> CompactionPicker::ComputeLevelScores(
    const std::vector<std::vector<FileMetaData>>& levels
) const {
    std::vector<CompactionLevelScore> scores;
    scores.reserve(levels.size());

    for (size_t l = 0; l < levels.size(); ++l) {
        CompactionLevelScore cls;
        cls.level = static_cast<int>(l);
        cls.target_bytes = TargetBytesForLevel(static_cast<int>(l));

        uint64_t total_bytes = 0;
        for (const auto& file : levels[l]) {
            total_bytes += file.file_size;
        }
        cls.bytes = total_bytes;

        if (l == 0) {
            // L0 score is based on file count
            if (options_.l0_file_num_compaction_trigger > 0) {
                cls.score = static_cast<double>(levels[0].size()) /
                            static_cast<double>(options_.l0_file_num_compaction_trigger);
            } else {
                cls.score = 0.0;
            }
        } else {
            // L1..Lmax score is based on byte size
            if (cls.target_bytes > 0) {
                cls.score = static_cast<double>(total_bytes) / static_cast<double>(cls.target_bytes);
            } else {
                cls.score = 0.0;
            }
        }
        scores.push_back(cls);
    }
    return scores;
}

bool CompactionPicker::PickCompaction(
    const std::vector<std::vector<FileMetaData>>& levels,
    const std::unordered_set<uint64_t>& files_being_compacted,
    LeveledCompactionJob* out_job
) const {
    if (!out_job || levels.empty()) return false;

    auto scores = ComputeLevelScores(levels);

    // Sort descending by score. In case of tie, lower level (e.g. L0) takes precedence.
    std::vector<size_t> indices(scores.size());
    for (size_t i = 0; i < scores.size(); ++i) indices[i] = i;

    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        if (scores[a].score != scores[b].score) {
            return scores[a].score > scores[b].score;
        }
        return scores[a].level < scores[b].level;
    });

    for (size_t idx : indices) {
        const auto& item = scores[idx];
        if (item.score < 1.0) {
            continue; // Score below threshold
        }

        int level = item.level;
        if (static_cast<size_t>(level + 1) >= levels.size()) {
            continue; // Cannot compact bottom-most level further
        }

        if (level == 0) {
            // Level 0 -> Level 1 compaction
            // Collect L0 candidate files not in files_being_compacted
            std::vector<FileMetaData> l0_candidates;
            for (const auto& f : levels[0]) {
                if (files_being_compacted.find(f.file_number) == files_being_compacted.end()) {
                    l0_candidates.push_back(f);
                }
            }
            if (l0_candidates.empty()) continue;

            // Compute overall range for candidate L0 files
            std::string smallest, largest;
            CompactionInputSelector::GetRange(l0_candidates, &smallest, &largest);

            // Find overlapping L1 files
            std::vector<FileMetaData> l1_overlapping;
            CompactionInputSelector::GetOverlappingInputs(
                Slice(smallest), Slice(largest), levels[1], &l1_overlapping
            );

            // Check if any L1 file is currently locked
            bool collision = false;
            for (const auto& f1 : l1_overlapping) {
                if (files_being_compacted.find(f1.file_number) != files_being_compacted.end()) {
                    collision = true;
                    break;
                }
            }
            if (collision) continue;

            out_job->level = 0;
            out_job->score = item.score;
            out_job->inputs_level = std::move(l0_candidates);
            out_job->inputs_next_level = std::move(l1_overlapping);
            out_job->output_files.clear();
            return true;
        } else {
            // Level i -> Level i+1 compaction (i >= 1)
            // Try picking an unreserved candidate file from Level i
            for (const auto& candidate_file : levels[level]) {
                if (files_being_compacted.find(candidate_file.file_number) != files_being_compacted.end()) {
                    continue;
                }

                std::vector<FileMetaData> level_inputs = { candidate_file };
                std::vector<FileMetaData> next_level_inputs;

                CompactionInputSelector::GetOverlappingInputs(
                    Slice(candidate_file.smallest_key),
                    Slice(candidate_file.largest_key),
                    levels[level + 1],
                    &next_level_inputs
                );

                // Expand Level i inputs safely if possible
                CompactionInputSelector::ExpandInputs(
                    levels[level], levels[level + 1], &level_inputs, &next_level_inputs
                );

                // Verify all picked level_inputs and next_level_inputs are free of reservation
                bool collision = false;
                for (const auto& f : level_inputs) {
                    if (files_being_compacted.find(f.file_number) != files_being_compacted.end()) {
                        collision = true;
                        break;
                    }
                }
                if (!collision) {
                    for (const auto& f : next_level_inputs) {
                        if (files_being_compacted.find(f.file_number) != files_being_compacted.end()) {
                            collision = true;
                            break;
                        }
                    }
                }

                if (!collision) {
                    out_job->level = level;
                    out_job->score = item.score;
                    out_job->inputs_level = std::move(level_inputs);
                    out_job->inputs_next_level = std::move(next_level_inputs);
                    out_job->output_files.clear();
                    return true;
                }
            }
        }
    }

    return false;
}

} // namespace lsm
