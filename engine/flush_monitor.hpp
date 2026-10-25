#pragma once

#include <atomic>
#include <functional>
#include <cstdint>
#include <cstddef>
#include "memtable_core.hpp"

namespace lsm {

class MemTableFlushMonitor {
public:
    using FlushCallback = std::function<void(MemTableCore* memtable, size_t mem_usage)>;

    explicit MemTableFlushMonitor(size_t write_buffer_size = 4 * 1024 * 1024);
    ~MemTableFlushMonitor() = default;

    // Checks memory usage of active memtable. If threshold is crossed, sets flush pending and triggers callback.
    bool CheckAndTrigger(MemTableCore* memtable);

    void SetThreshold(size_t write_buffer_size) {
        write_buffer_size_.store(write_buffer_size, std::memory_order_relaxed);
    }

    size_t GetThreshold() const {
        return write_buffer_size_.load(std::memory_order_relaxed);
    }

    void RegisterCallback(FlushCallback cb) {
        callback_ = std::move(cb);
    }

    uint64_t GetTotalFlushSignalsEmitted() const {
        return flush_signals_count_.load(std::memory_order_relaxed);
    }

    void ResetStats() {
        flush_signals_count_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<size_t> write_buffer_size_{4 * 1024 * 1024};
    std::atomic<uint64_t> flush_signals_count_{0};
    FlushCallback callback_{};
};

} // namespace lsm
