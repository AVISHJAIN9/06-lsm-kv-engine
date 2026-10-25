#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "write_batch.hpp"
#include "wal_manager.hpp"
#include "metrics.hpp"

namespace lsm {

enum class FsyncMode : uint8_t {
    kFsyncNever = 0,        // OS page cache, asynchronous flushing
    kFsyncEveryWrite = 1,   // Synchronous fsync per commit
    kFsyncGroupCommit = 2   // Group-committed batched fsync
};

class GroupCommitter {
public:
    struct WriteRequest {
        WriteBatch* batch{nullptr};
        bool sync{false};
        Status status{Status::OK()};
        std::atomic<bool> done{false};
    };

    using PrepareCallback = std::function<void(WriteBatch* batch)>;
    using ApplyCallback = std::function<Status(WriteBatch* batch)>;

    GroupCommitter(
        WALManager* wal_manager,
        FsyncMode mode,
        PrepareCallback prepare_cb,
        ApplyCallback apply_cb
    );
    ~GroupCommitter();

    GroupCommitter(const GroupCommitter&) = delete;
    GroupCommitter& operator=(const GroupCommitter&) = delete;

    Status Submit(WriteBatch* batch, bool sync);
    void SetFsyncMode(FsyncMode mode) { fsync_mode_.store(mode); }
    FsyncMode GetFsyncMode() const { return fsync_mode_.load(); }

    uint64_t GetGroupCommitCount() const { return group_commit_count_.load(); }
    uint64_t GetTotalBatchesProcessed() const { return total_batches_processed_.load(); }

private:
    WALManager* wal_manager_{nullptr};
    std::atomic<FsyncMode> fsync_mode_{FsyncMode::kFsyncNever};
    PrepareCallback prepare_callback_{};
    ApplyCallback apply_callback_{};

    std::mutex queue_mutex_{};
    std::condition_variable queue_cv_{};
    std::deque<WriteRequest*> pending_queue_{};
    bool leader_active_{false};

    std::atomic<uint64_t> group_commit_count_{0};
    std::atomic<uint64_t> total_batches_processed_{0};

    void ProcessGroup(const std::vector<WriteRequest*>& group);
};

} // namespace lsm
