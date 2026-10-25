#include "compaction_token.hpp"
#include <algorithm>

namespace fs = std::filesystem;

namespace lsm {

void CompactionTempFileTracker::RegisterFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mu_);
    files_.push_back(filepath);
}

void CompactionTempFileTracker::UnregisterFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mu_);
    files_.erase(
        std::remove(files_.begin(), files_.end(), filepath),
        files_.end()
    );
}

void CompactionTempFileTracker::CommitAll() {
    std::lock_guard<std::mutex> lock(mu_);
    files_.clear();
    auto_cleanup_ = false;
}

void CompactionTempFileTracker::Cleanup() {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& f : files_) {
        std::error_code ec;
        if (fs::exists(f, ec)) {
            fs::remove(f, ec);
        }
        // Also check if .tmp version exists
        std::string tmp_f = f + ".tmp";
        if (fs::exists(tmp_f, ec)) {
            fs::remove(tmp_f, ec);
        }
    }
    files_.clear();
}

std::vector<std::string> CompactionTempFileTracker::GetTrackedFiles() const {
    std::lock_guard<std::mutex> lock(mu_);
    return files_;
}

size_t CompactionTempFileTracker::TrackedCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return files_.size();
}

} // namespace lsm
