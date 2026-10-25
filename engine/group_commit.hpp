#pragma once

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "write_batch.hpp"
#include "wal_writer.hpp"
#include "metrics.hpp"

namespace lsm {

enum WriteState : uint8_t {
    kStateInit = 0,
    kStateQueued = 1,
    kStateLeader = 2,
    kStateCompleted = 3
};

struct WriteThreadNode {
    WriteBatch* batch{nullptr};
    bool sync{false};
    Status status{Status::OK()};
    std::atomic<uint8_t> state{kStateInit};
    WriteThreadNode* next{nullptr};
    std::condition_variable cv{};
    std::mutex mutex{};
};

// Lock-Free Intrusive MPSC Queue for Group Commit
class LockFreeGroupCommitter {
public:
    using PrepareBatchCallback = std::function<void(WriteBatch* batch)>;
    using ApplyBatchCallback = std::function<Status(WriteBatch* batch)>;

    LockFreeGroupCommitter(
        WALWriter* wal_writer,
        PrepareBatchCallback prepare_cb,
        ApplyBatchCallback apply_cb
    );
    ~LockFreeGroupCommitter() = default;

    LockFreeGroupCommitter(const LockFreeGroupCommitter&) = delete;
    LockFreeGroupCommitter& operator=(const LockFreeGroupCommitter&) = delete;

    Status Submit(WriteBatch* batch, bool sync);
    void UpdateWALWriter(WALWriter* wal_writer);

    uint64_t GetTotalBatchesProcessed() const { return total_batches_.load(std::memory_order_relaxed); }
    uint64_t GetTotalGroupSyncs() const { return total_syncs_.load(std::memory_order_relaxed); }

private:
    std::atomic<WALWriter*> wal_writer_{nullptr};
    PrepareBatchCallback prepare_callback_{};
    ApplyBatchCallback apply_callback_{};

    std::atomic<WriteThreadNode*> head_{nullptr};
    std::mutex writer_mutex_{};

    std::atomic<uint64_t> total_batches_{0};
    std::atomic<uint64_t> total_syncs_{0};

    void Push(WriteThreadNode* node);
    WriteThreadNode* DrainList();
    static WriteThreadNode* ReverseList(WriteThreadNode* head);
    void ProcessLeaderGroup(WriteThreadNode* leader_node);
};

} // namespace lsm
