#pragma once

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <memory>
#include <functional>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "write_batch.hpp"
#include "memtable_queue.hpp"
#include "wal_writer.hpp"
#include "write_stall_controller.hpp"
#include "write_buffer_manager.hpp"

namespace lsm {

struct WriterContext {
    WriteBatch* batch{nullptr};
    bool sync{false};
    std::atomic<bool> done{false};
    Status status{Status::OK()};
    std::mutex mu{};
    std::condition_variable cv{};
    WriterContext* next{nullptr};
};

struct WritePipelineStats {
    uint64_t total_client_writes{0};
    uint64_t total_coalesced_groups{0};
    uint64_t total_batches_coalesced{0};
    uint64_t total_wal_syncs{0};
};

class WritePipeline {
public:
    WritePipeline(
        MemTableQueue* mem_queue,
        WALWriter* wal_writer,
        WriteStallController* stall_controller = nullptr,
        WriteBufferManager* buffer_manager = nullptr
    );
    ~WritePipeline() = default;

    WritePipeline(const WritePipeline&) = delete;
    WritePipeline& operator=(const WritePipeline&) = delete;

    // Submits a write batch to the pipeline. Coalesces concurrent mutator writes into
    // single atomic I/O operations with linearizable sequence numbers.
    Status Write(WriteBatch* batch, bool sync);

    void UpdateWALWriter(WALWriter* wal_writer);

    SequenceNumber GetLastSequence() const {
        return last_sequence_.load(std::memory_order_relaxed);
    }
    void SetLastSequence(SequenceNumber seq) {
        last_sequence_.store(seq, std::memory_order_release);
    }

    WritePipelineStats GetStats() const;
    void ResetStats();

private:
    MemTableQueue* mem_queue_{nullptr};
    std::atomic<WALWriter*> wal_writer_{nullptr};
    WriteStallController* stall_controller_{nullptr};
    WriteBufferManager* buffer_manager_{nullptr};

    std::atomic<SequenceNumber> last_sequence_{1};

    std::atomic<WriterContext*> queue_head_{nullptr};
    std::mutex leader_mutex_{};
    std::condition_variable cv_stall_{};

    mutable std::mutex stats_mu_{};
    WritePipelineStats stats_{};

    void PushWriter(WriterContext* w);
    WriterContext* DrainWriters();
    static WriterContext* ReverseWriters(WriterContext* head);
    void ProcessLeaderBatch(WriterContext* leader_w);
};

} // namespace lsm
