#pragma once

#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <filesystem>

namespace lsm {

// -----------------------------------------------------------------------------
// CancellationToken: Thread-safe cooperative cancellation signaling
// -----------------------------------------------------------------------------
class CancellationToken {
public:
    CancellationToken() = default;

    void Cancel() {
        cancelled_.store(true, std::memory_order_release);
    }

    bool IsCancelled() const {
        return cancelled_.load(std::memory_order_acquire);
    }

    void Reset() {
        cancelled_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> cancelled_{false};
};

// -----------------------------------------------------------------------------
// CompactionTempFileTracker: Tracks in-flight files and guarantees cleanup on abort
// -----------------------------------------------------------------------------
class CompactionTempFileTracker {
public:
    CompactionTempFileTracker() = default;
    ~CompactionTempFileTracker() {
        if (auto_cleanup_) {
            Cleanup();
        }
    }

    void RegisterFile(const std::string& filepath);
    void UnregisterFile(const std::string& filepath);
    void CommitAll();
    void Cleanup();

    std::vector<std::string> GetTrackedFiles() const;
    size_t TrackedCount() const;

private:
    mutable std::mutex mu_;
    std::vector<std::string> files_;
    bool auto_cleanup_{true};
};

} // namespace lsm
