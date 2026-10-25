#pragma once

#include <unordered_set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <cstdint>

namespace lsm {

// -----------------------------------------------------------------------------
// CompactionConcurrencyGate: Non-blocking atomic file reservation tracker
// Guarantees zero dual-compaction collisions across concurrent worker threads
// -----------------------------------------------------------------------------
class CompactionConcurrencyGate {
public:
    CompactionConcurrencyGate() = default;
    ~CompactionConcurrencyGate() = default;

    CompactionConcurrencyGate(const CompactionConcurrencyGate&) = delete;
    CompactionConcurrencyGate& operator=(const CompactionConcurrencyGate&) = delete;

    // Atomically attempts to lock all given file IDs.
    // Returns true if all files were successfully reserved; false if any file was already locked.
    bool TryLockFiles(const std::vector<uint64_t>& file_ids);

    // Releases reservation on given file IDs and awakens waiting workers
    void UnlockFiles(const std::vector<uint64_t>& file_ids);

    // Checks if a specific file ID is currently locked
    bool IsFileBeingCompacted(uint64_t file_id) const;

    // Returns a snapshot copy of all active file reservations
    std::unordered_set<uint64_t> GetLockedFiles() const;

    size_t ActiveLockedFilesCount() const;

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_set<uint64_t> files_being_compacted_;
};

} // namespace lsm
