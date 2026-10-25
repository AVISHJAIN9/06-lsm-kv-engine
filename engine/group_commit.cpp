#include "group_commit.hpp"
#include <thread>
#include <chrono>

namespace lsm {

LockFreeGroupCommitter::LockFreeGroupCommitter(
    WALWriter* wal_writer,
    PrepareBatchCallback prepare_cb,
    ApplyBatchCallback apply_cb
) : wal_writer_(wal_writer),
    prepare_callback_(std::move(prepare_cb)),
    apply_callback_(std::move(apply_cb)) {}

void LockFreeGroupCommitter::UpdateWALWriter(WALWriter* wal_writer) {
    wal_writer_.store(wal_writer, std::memory_order_release);
}

void LockFreeGroupCommitter::Push(WriteThreadNode* node) {
    WriteThreadNode* old_head = head_.load(std::memory_order_relaxed);
    do {
        node->next = old_head;
    } while (!head_.compare_exchange_weak(
        old_head, node, std::memory_order_release, std::memory_order_relaxed));
}

WriteThreadNode* LockFreeGroupCommitter::DrainList() {
    return head_.exchange(nullptr, std::memory_order_acq_rel);
}

WriteThreadNode* LockFreeGroupCommitter::ReverseList(WriteThreadNode* head) {
    WriteThreadNode* prev = nullptr;
    WriteThreadNode* curr = head;
    while (curr != nullptr) {
        WriteThreadNode* next = curr->next;
        curr->next = prev;
        prev = curr;
        curr = next;
    }
    return prev;
}

Status LockFreeGroupCommitter::Submit(WriteBatch* batch, bool sync) {
    if (!batch || batch->Count() == 0) {
        return Status::OK();
    }

    WriteThreadNode node;
    node.batch = batch;
    node.sync = sync;
    node.state.store(kStateQueued, std::memory_order_release);

    WriteThreadNode* old_head = head_.load(std::memory_order_relaxed);
    do {
        node.next = old_head;
    } while (!head_.compare_exchange_weak(
        old_head, &node, std::memory_order_release, std::memory_order_relaxed));

    bool is_leader = (old_head == nullptr);

    if (is_leader) {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        ProcessLeaderGroup(&node);
    } else {
        std::unique_lock<std::mutex> lock(node.mutex);
        node.cv.wait(lock, [&node] {
            return node.state.load(std::memory_order_acquire) == kStateCompleted;
        });
    }

    return node.status;
}

void LockFreeGroupCommitter::ProcessLeaderGroup(WriteThreadNode* leader_node) {
    WriteThreadNode* raw_list = DrainList();
    if (!raw_list) {
        return;
    }

    // Reverse the atomic LIFO stack into FIFO order
    WriteThreadNode* fifo_list = ReverseList(raw_list);

    bool should_sync = false;
    uint64_t batch_count = 0;
    Status group_status = Status::OK();
    WALWriter* writer = wal_writer_.load(std::memory_order_acquire);

    // 1. Prepare/sequence all batches in FIFO order first
    if (prepare_callback_) {
        WriteThreadNode* curr = fifo_list;
        while (curr != nullptr) {
            if (curr->batch) {
                prepare_callback_(curr->batch);
            }
            curr = curr->next;
        }
    }

    // 2. Write sequenced records to WAL
    WriteThreadNode* curr = fifo_list;
    while (curr != nullptr) {
        if (curr->sync) {
            should_sync = true;
        }
        if (writer && curr->batch) {
            Slice record(curr->batch->Data());
            Status s = writer->AddRecord(record);
            if (!s.ok()) {
                group_status = s;
                break;
            }
        }
        batch_count++;
        curr = curr->next;
    }

    // 3. Perform batched fsync if required
    if (group_status.ok() && should_sync && writer) {
        group_status = writer->Sync();
        if (group_status.ok()) {
            total_syncs_.fetch_add(1, std::memory_order_relaxed);
            MetricsCollector::Instance().AddCounter(CounterType::kWalSyncCount, 1);
        }
    }

    // 4. Apply batches to active MemTable
    if (group_status.ok() && apply_callback_) {
        curr = fifo_list;
        while (curr != nullptr) {
            if (curr->batch) {
                Status s = apply_callback_(curr->batch);
                if (!s.ok()) {
                    group_status = s;
                    break;
                }
            }
            curr = curr->next;
        }
    }

    total_batches_.fetch_add(batch_count, std::memory_order_relaxed);
    MetricsCollector::Instance().AddCounter(CounterType::kGroupCommitBatches, 1);
    MetricsCollector::Instance().AddCounter(CounterType::kOpsBatchWrite, batch_count);

    // 5. Mark all follower nodes completed and notify them
    curr = fifo_list;
    while (curr != nullptr) {
        WriteThreadNode* next = curr->next;
        curr->status = group_status;
        curr->state.store(kStateCompleted, std::memory_order_release);
        if (curr != leader_node) {
            std::lock_guard<std::mutex> lock(curr->mutex);
            curr->cv.notify_one();
        }
        curr = next;
    }
}

} // namespace lsm
