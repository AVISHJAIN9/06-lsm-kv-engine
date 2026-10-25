#pragma once

#include <string>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include "status.hpp"
#include "memtable_core.hpp"

namespace lsm {

struct FlushTask {
    std::shared_ptr<MemTableCore> mem{nullptr};
    uint64_t log_number{0};
    std::string sst_output_path{""};
    std::function<void(Status, uint64_t, const std::string&)> on_complete{};
};

struct FlushStats {
    uint64_t flushes_completed{0};
    uint64_t flushes_failed{0};
    uint64_t total_keys_flushed{0};
    uint64_t total_bytes_flushed{0};
    uint64_t total_flush_duration_micros{0};
};

class FlushScheduler {
public:
    explicit FlushScheduler(std::string db_directory);
    ~FlushScheduler();

    FlushScheduler(const FlushScheduler&) = delete;
    FlushScheduler& operator=(const FlushScheduler&) = delete;

    // Enqueues an immutable memtable for background Level-0 SSTable flush
    Status ScheduleFlush(
        std::shared_ptr<MemTableCore> mem,
        uint64_t log_number,
        std::function<void(Status status, uint64_t log_num, const std::string& sst_path)> on_complete = nullptr
    );

    // Blocks until all currently queued flush jobs are completed
    void WaitForFlush();

    // Stops background worker gracefully
    void Shutdown();

    // Query stats
    FlushStats GetStats() const;
    size_t PendingFlushCount() const;

private:
    std::string db_dir_{};
    std::deque<FlushTask> queue_{};
    mutable std::mutex mu_{};
    std::condition_variable cv_work_{};
    std::condition_variable cv_flush_done_{};

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> is_flushing_{false};
    std::atomic<uint64_t> next_file_number_{1};

    std::thread worker_{};
    FlushStats stats_{};

    void WorkerLoop();
    Status FlushMemTableToSST(const FlushTask& task, std::string* out_sst_path, uint64_t* out_keys, uint64_t* out_bytes);
};

} // namespace lsm
