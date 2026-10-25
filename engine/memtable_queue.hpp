#pragma once

#include <memory>
#include <deque>
#include <vector>
#include <shared_mutex>
#include <mutex>
#include <atomic>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "memtable_core.hpp"

namespace lsm {

class MemTableQueue {
public:
    explicit MemTableQueue(size_t write_buffer_size = 4 * 1024 * 1024);
    ~MemTableQueue() = default;

    MemTableQueue(const MemTableQueue&) = delete;
    MemTableQueue& operator=(const MemTableQueue&) = delete;

    // Point lookup across active memtable followed by immutable queue (newest to oldest)
    bool Get(const Slice& user_key, SequenceNumber snapshot_seq, std::string* value, Status* s) const;

    // Writes to active memtable
    void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

    // Atomically switches active memtable to immutable, pushes old active to imm_queue,
    // and instantiates fresh active memtable with the given new WAL log number.
    Status SwitchMemtable(uint64_t new_log_number, std::shared_ptr<MemTableCore>* out_imm = nullptr);

    // Accessors
    std::shared_ptr<MemTableCore> GetActive() const;
    std::vector<std::shared_ptr<MemTableCore>> GetImmutableQueue() const;
    size_t ImmQueueSize() const;

    // Pops oldest immutable memtable upon flush completion
    Status PopOldestImmutable(std::shared_ptr<MemTableCore>* out_popped = nullptr);

    // Total memory consumption across active and immutable memtables
    size_t TotalMemoryUsage() const;

    // Checks if active memtable has exceeded its capacity
    bool IsActiveFlushPending() const;

    uint64_t GetActiveLogNumber() const { return active_log_number_.load(std::memory_order_relaxed); }
    void SetActiveLogNumber(uint64_t log_num) { active_log_number_.store(log_num, std::memory_order_relaxed); }

private:
    size_t write_buffer_size_{4 * 1024 * 1024};
    mutable std::shared_mutex rw_mutex_{};

    std::shared_ptr<MemTableCore> active_{nullptr};
    std::deque<std::shared_ptr<MemTableCore>> imm_queue_{};

    std::atomic<uint64_t> active_log_number_{1};
    std::atomic<size_t> imm_count_{0};
};

} // namespace lsm
