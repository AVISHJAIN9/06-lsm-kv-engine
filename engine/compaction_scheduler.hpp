#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>
#include "leveled_compaction.hpp"
#include "compaction_concurrency.hpp"
#include "compaction_executor.hpp"
#include "status.hpp"

namespace lsm {

struct ScheduledCompactionTask {
    LeveledCompactionJob job;
    std::string db_dir;
    uint64_t* next_file_number{nullptr};
    std::function<void(const Status& status, const LeveledCompactionJob& job)> on_complete;

    bool operator<(const ScheduledCompactionTask& other) const {
        if (job.score != other.job.score) {
            return job.score < other.job.score; // Max score at top
        }
        return job.level > other.job.level;     // Lower level (e.g. L0) preferred
    }
};

// -----------------------------------------------------------------------------
// CompactionScheduler: Multi-Worker Priority Queue & Compaction Coordinator
// -----------------------------------------------------------------------------
class CompactionScheduler {
public:
    CompactionScheduler(
        size_t num_workers,
        std::shared_ptr<CompactionConcurrencyGate> concurrency_gate,
        std::shared_ptr<WriteAmplificationTracker> waf_tracker = nullptr
    );
    ~CompactionScheduler();

    CompactionScheduler(const CompactionScheduler&) = delete;
    CompactionScheduler& operator=(const CompactionScheduler&) = delete;

    // Enqueues a compaction task into the priority queue
    bool ScheduleCompaction(ScheduledCompactionTask task);

    // Blocks until all queued tasks and active workers finish
    void WaitForCompletion();

    // Initiates shutdown and joins worker threads
    void Shutdown();

    size_t PendingTasksCount() const;
    size_t ActiveWorkersCount() const;
    size_t CompletedTasksCount() const;

private:
    void WorkerLoop();

    size_t num_workers_;
    std::shared_ptr<CompactionConcurrencyGate> concurrency_gate_;
    std::shared_ptr<WriteAmplificationTracker> waf_tracker_;

    mutable std::mutex mu_;
    std::condition_variable cv_task_;
    std::condition_variable cv_idle_;

    std::priority_queue<ScheduledCompactionTask> task_queue_;
    std::vector<std::thread> workers_;

    std::atomic<bool> shutdown_{false};
    std::atomic<size_t> active_workers_{0};
    std::atomic<size_t> completed_tasks_{0};
};

} // namespace lsm
