#include "compaction_concurrency.hpp"

namespace lsm {

bool CompactionConcurrencyGate::TryLockFiles(const std::vector<uint64_t>& file_ids) {
    std::lock_guard<std::mutex> lock(mu_);

    for (uint64_t fid : file_ids) {
        if (files_being_compacted_.find(fid) != files_being_compacted_.end()) {
            return false; // Collision detected: at least one file is currently being compacted
        }
    }

    for (uint64_t fid : file_ids) {
        files_being_compacted_.insert(fid);
    }
    return true;
}

void CompactionConcurrencyGate::UnlockFiles(const std::vector<uint64_t>& file_ids) {
    std::lock_guard<std::mutex> lock(mu_);

    for (uint64_t fid : file_ids) {
        files_being_compacted_.erase(fid);
    }
    cv_.notify_all();
}

bool CompactionConcurrencyGate::IsFileBeingCompacted(uint64_t file_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    return files_being_compacted_.find(file_id) != files_being_compacted_.end();
}

std::unordered_set<uint64_t> CompactionConcurrencyGate::GetLockedFiles() const {
    std::lock_guard<std::mutex> lock(mu_);
    return files_being_compacted_;
}

size_t CompactionConcurrencyGate::ActiveLockedFilesCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return files_being_compacted_.size();
}

} // namespace lsm
