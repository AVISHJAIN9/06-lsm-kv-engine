#include "compaction_debt.hpp"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace lsm {

CompactionDebtTracker::CompactionDebtTracker(
    LeveledCompactionOptions options,
    uint64_t danger_threshold_bytes
) : options_(options), danger_threshold_bytes_(danger_threshold_bytes) {}

uint64_t CompactionDebtTracker::TargetBytesForLevel(int level) const {
    if (level <= 0) {
        return options_.l0_file_num_compaction_trigger * options_.target_file_size;
    }
    double multiplier = std::pow(options_.level_multiplier, level - 1);
    return static_cast<uint64_t>(static_cast<double>(options_.base_level_bytes) * multiplier);
}

CompactionDebtReport CompactionDebtTracker::CalculateDebt(
    const std::vector<std::vector<FileMetaData>>& levels
) const {
    CompactionDebtReport report;
    report.level_debt_bytes.resize(levels.size(), 0);
    report.level_actual_bytes.resize(levels.size(), 0);
    report.level_target_bytes.resize(levels.size(), 0);
    report.total_debt_bytes = 0;

    for (size_t l = 0; l < levels.size(); ++l) {
        uint64_t actual_bytes = 0;
        for (const auto& file : levels[l]) {
            actual_bytes += file.file_size;
        }
        report.level_actual_bytes[l] = actual_bytes;
        report.level_target_bytes[l] = TargetBytesForLevel(static_cast<int>(l));

        uint64_t debt = 0;
        if (l == 0) {
            if (levels[0].size() > options_.l0_file_num_compaction_trigger) {
                uint64_t excess_files = levels[0].size() - options_.l0_file_num_compaction_trigger;
                uint64_t avg_file_size = (levels[0].empty() ? options_.target_file_size : (actual_bytes / levels[0].size()));
                debt = excess_files * avg_file_size;
            }
        } else {
            if (actual_bytes > report.level_target_bytes[l]) {
                debt = actual_bytes - report.level_target_bytes[l];
            }
        }

        report.level_debt_bytes[l] = debt;
        report.total_debt_bytes += debt;
    }

    report.danger_watermark_crossed = (report.total_debt_bytes >= danger_threshold_bytes_);
    return report;
}

std::string CompactionDebtReport::ToString() const {
    std::ostringstream oss;
    oss << "Compaction Debt & Backlog Report:\n"
        << "  - Total Outstanding Debt:   " << total_debt_bytes << " bytes ("
        << (total_debt_bytes / (1024.0 * 1024.0)) << " MB)\n"
        << "  - Danger Watermark Crossed: " << (danger_watermark_crossed ? "YES (STALL IMMINENT)" : "NO") << "\n";
    for (size_t i = 0; i < level_debt_bytes.size(); ++i) {
        oss << "    * Level " << i << ": Actual=" << (level_actual_bytes[i] / 1024) << "KB, Target="
            << (level_target_bytes[i] / 1024) << "KB, Debt=" << (level_debt_bytes[i] / 1024) << "KB\n";
    }
    return oss.str();
}

} // namespace lsm
