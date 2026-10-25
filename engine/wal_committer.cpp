#include "wal_committer.hpp"

namespace lsm {

GroupCommitter::GroupCommitter(
    WALManager* wal_manager,
    FsyncMode mode,
    PrepareCallback prepare_cb,
    ApplyCallback apply_cb
) : wal_manager_(wal_manager),
    fsync_mode_(mode),
    prepare_callback_(std::move(prepare_cb)),
    apply_callback_(std::move(apply_cb)) {}

GroupCommitter::~GroupCommitter() = default;

Status GroupCommitter::Submit(WriteBatch* batch, bool sync) {
    if (!batch || batch->Count() == 0) {
        return Status::OK();
    }

    WriteRequest req;
    req.batch = batch;
    req.sync = sync;

    std::unique_lock<std::mutex> lock(queue_mutex_);
    pending_queue_.push_back(&req);

    while (!req.done.load(std::memory_order_acquire)) {
        if (!leader_active_ && pending_queue_.front() == &req) {
            // Current thread becomes group leader
            leader_active_ = true;

            std::vector<WriteRequest*> group;
            while (!pending_queue_.empty() && group.size() < 128) {
                group.push_back(pending_queue_.front());
                pending_queue_.pop_front();
            }

            lock.unlock();

            ProcessGroup(group);

            lock.lock();
            for (auto* r : group) {
                r->done.store(true, std::memory_order_release);
            }
            leader_active_ = false;
            queue_cv_.notify_all();
            break;
        } else {
            queue_cv_.wait(lock);
        }
    }

    return req.status;
}

void GroupCommitter::ProcessGroup(const std::vector<WriteRequest*>& group) {
    if (group.empty()) return;

    ScopedTimer timer(&MetricsCollector::Instance(), MetricType::kWalAppendLatency);

    bool should_sync = false;
    FsyncMode mode = fsync_mode_.load();
    if (mode == FsyncMode::kFsyncEveryWrite || mode == FsyncMode::kFsyncGroupCommit) {
        should_sync = true;
    }

    Status group_status = Status::OK();

    // 1. Assign sequence numbers to all batches first
    if (prepare_callback_) {
        for (auto* req : group) {
            prepare_callback_(req->batch);
        }
    }

    // 2. Write all sequenced batches to WAL
    if (wal_manager_) {
        for (auto* req : group) {
            if (req->sync) {
                should_sync = true;
            }
            Slice record(req->batch->Data());
            Status s = wal_manager_->Append(record, false);
            if (!s.ok()) {
                group_status = s;
                break;
            }
        }

        if (group_status.ok() && should_sync) {
            ScopedTimer sync_timer(&MetricsCollector::Instance(), MetricType::kWalSyncLatency);
            group_status = wal_manager_->Sync();
        }
    }

    // 3. Apply batches in-memory via apply callback
    if (group_status.ok() && apply_callback_) {
        for (auto* req : group) {
            Status s = apply_callback_(req->batch);
            if (!s.ok()) {
                group_status = s;
                break;
            }
        }
    }

    group_commit_count_.fetch_add(1, std::memory_order_relaxed);
    total_batches_processed_.fetch_add(group.size(), std::memory_order_relaxed);
    MetricsCollector::Instance().AddCounter(CounterType::kGroupCommitBatches, 1);
    MetricsCollector::Instance().AddCounter(CounterType::kOpsBatchWrite, group.size());

    // 4. Populate return status for all group participants
    for (auto* req : group) {
        req->status = group_status;
    }
}

} // namespace lsm
