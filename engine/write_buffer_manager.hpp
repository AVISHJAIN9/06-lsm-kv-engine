#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>

namespace lsm {

class WriteBufferManager {
public:
    explicit WriteBufferManager(size_t buffer_size_limit = 64 * 1024 * 1024);
    ~WriteBufferManager() = default;

    // Reserves memory for a memtable insertion.
    // Returns true if global memory usage crosses the flush threshold (90% of budget),
    // signaling that an early proactive flush is required.
    bool ReserveMemory(size_t bytes);

    // Releases memory once an immutable memtable has materialized into SSTable and is freed.
    void ReleaseMemory(size_t bytes);

    // Checks if global write buffer budget is strictly exceeded
    bool IsBudgetExceeded() const;

    // Checks if memory usage warrants initiating a proactive background flush
    bool ShouldFlush() const;

    size_t MemoryUsage() const { return memory_allocated_.load(std::memory_order_relaxed); }
    size_t GetLimit() const { return buffer_size_limit_.load(std::memory_order_relaxed); }
    size_t GetFlushThreshold() const { return flush_threshold_.load(std::memory_order_relaxed); }

    void SetLimit(size_t buffer_size_limit);
    void Reset();

private:
    std::atomic<size_t> buffer_size_limit_{64 * 1024 * 1024};
    std::atomic<size_t> flush_threshold_{static_cast<size_t>(64 * 1024 * 1024 * 0.90)};
    std::atomic<size_t> memory_allocated_{0};
};

} // namespace lsm
