#include "write_pipeline.hpp"
#include <chrono>

namespace lsm {

WritePipeline::WritePipeline(
    MemTableQueue* mem_queue,
    WALWriter* wal_writer,
    WriteStallController* stall_controller,
    WriteBufferManager* buffer_manager
) : mem_queue_(mem_queue),
    wal_writer_(wal_writer),
    stall_controller_(stall_controller),
    buffer_manager_(buffer_manager) {}

void WritePipeline::UpdateWALWriter(WALWriter* wal_writer) {
    wal_writer_.store(wal_writer, std::memory_order_release);
}

void WritePipeline::PushWriter(WriterContext* w) {
    WriterContext* old_head = queue_head_.load(std::memory_order_relaxed);
    do {
        w->next = old_head;
    } while (!queue_head_.compare_exchange_weak(
        old_head, w, std::memory_order_release, std::memory_order_relaxed));
}

WriterContext* WritePipeline::DrainWriters() {
    return queue_head_.exchange(nullptr, std::memory_order_acq_rel);
}

WriterContext* WritePipeline::ReverseWriters(WriterContext* head) {
    WriterContext* prev = nullptr;
    WriterContext* curr = head;
    while (curr != nullptr) {
        WriterContext* next = curr->next;
        curr->next = prev;
        prev = curr;
        curr = next;
    }
    return prev;
}

Status WritePipeline::Write(WriteBatch* batch, bool sync) {
    if (!batch || batch->Count() == 0) {
        return Status::OK();
    }

    // 1. Check adaptive write stall backpressure if configured
    if (stall_controller_ && mem_queue_) {
        std::unique_lock<std::mutex> lock(leader_mutex_, std::defer_lock);
        stall_controller_->ApplyBackpressure(
            mem_queue_->ImmQueueSize(),
            0, // L0 file count
            cv_stall_,
            lock
        );
    }

    // 2. Track global write buffer memory
    if (buffer_manager_) {
        buffer_manager_->ReserveMemory(batch->ByteSize());
    }

    WriterContext w;
    w.batch = batch;
    w.sync = sync;
    w.done.store(false, std::memory_order_release);

    WriterContext* old_head = queue_head_.load(std::memory_order_relaxed);
    do {
        w.next = old_head;
    } while (!queue_head_.compare_exchange_weak(
        old_head, &w, std::memory_order_release, std::memory_order_relaxed));

    bool is_leader = (old_head == nullptr);

    if (is_leader) {
        std::lock_guard<std::mutex> lock(leader_mutex_);
        ProcessLeaderBatch(&w);
    } else {
        std::unique_lock<std::mutex> lock(w.mu);
        w.cv.wait(lock, [&w] {
            return w.done.load(std::memory_order_acquire);
        });
    }

    return w.status;
}

class MemTableInserter : public WriteBatch::Handler {
public:
    MemTableInserter(SequenceNumber seq, MemTableQueue* queue)
        : sequence_(seq), queue_(queue) {}

    void Put(const Slice& key, const Slice& value) override {
        queue_->Add(sequence_, ValueType::kTypeValue, key, value);
        sequence_++;
    }

    void Delete(const Slice& key) override {
        queue_->Add(sequence_, ValueType::kTypeDeletion, key, "");
        sequence_++;
    }

private:
    SequenceNumber sequence_;
    MemTableQueue* queue_;
};

void WritePipeline::ProcessLeaderBatch(WriterContext* leader_w) {
    WriterContext* raw_list = DrainWriters();
    if (!raw_list) {
        return;
    }

    WriterContext* fifo_list = ReverseWriters(raw_list);

    // 1. Calculate total entries and count batches
    uint32_t total_entries = 0;
    uint64_t batch_count = 0;
    bool should_sync = false;

    WriterContext* curr = fifo_list;
    while (curr != nullptr) {
        if (curr->batch) {
            total_entries += curr->batch->Count();
        }
        if (curr->sync) {
            should_sync = true;
        }
        batch_count++;
        curr = curr->next;
    }

    // 2. Reserve linearizable sequence number chunk
    SequenceNumber assigned_seq = last_sequence_.fetch_add(total_entries, std::memory_order_relaxed);
    SequenceNumber cur_seq = assigned_seq;

    Status group_status = Status::OK();
    WALWriter* writer = wal_writer_.load(std::memory_order_acquire);

    // 3. Set sequence numbers and write to WAL
    curr = fifo_list;
    while (curr != nullptr) {
        if (curr->batch) {
            curr->batch->SetSequence(cur_seq);
            cur_seq += curr->batch->Count();

            if (writer) {
                Slice record(curr->batch->Data());
                Status s = writer->AddRecord(record);
                if (!s.ok()) {
                    group_status = s;
                    break;
                }
            }
        }
        curr = curr->next;
    }

    // 4. Batched sync if required
    if (group_status.ok() && should_sync && writer) {
        group_status = writer->Sync();
        if (group_status.ok()) {
            std::lock_guard<std::mutex> lock(stats_mu_);
            stats_.total_wal_syncs++;
        }
    }

    // 5. Apply all records to active MemTable
    if (group_status.ok() && mem_queue_) {
        curr = fifo_list;
        while (curr != nullptr) {
            if (curr->batch) {
                SequenceNumber batch_seq = curr->batch->Sequence();
                MemTableInserter inserter(batch_seq, mem_queue_);
                curr->batch->Iterate(&inserter);
            }
            curr = curr->next;
        }
    }

    {
        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_.total_client_writes += batch_count;
        stats_.total_coalesced_groups++;
        stats_.total_batches_coalesced += batch_count;
    }

    // 6. Notify all followers
    curr = fifo_list;
    while (curr != nullptr) {
        WriterContext* next = curr->next;
        curr->status = group_status;
        curr->done.store(true, std::memory_order_release);
        if (curr != leader_w) {
            std::lock_guard<std::mutex> lock(curr->mu);
            curr->cv.notify_one();
        }
        curr = next;
    }
}

WritePipelineStats WritePipeline::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mu_);
    return stats_;
}

void WritePipeline::ResetStats() {
    std::lock_guard<std::mutex> lock(stats_mu_);
    stats_ = WritePipelineStats{};
}

} // namespace lsm
