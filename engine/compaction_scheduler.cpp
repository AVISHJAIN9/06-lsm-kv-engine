#include "compaction_scheduler.hpp"
#include <iostream>

namespace lsm {

CompactionScheduler::CompactionScheduler(
    size_t num_workers,
    std::shared_ptr<CompactionConcurrencyGate> concurrency_gate,
    std::shared_ptr<WriteAmplificationTracker> waf_tracker
) : num_workers_(num_workers == 0 ? 1 : num_workers),
    concurrency_gate_(std::move(concurrency_gate)),
    waf_tracker_(std::move(waf_tracker)) {
    workers_.reserve(num_workers_);
    for (size_t i = 0; i < num_workers_; ++i) {
        workers_.emplace_back(&CompactionScheduler::WorkerLoop, this);
    }
}

CompactionScheduler::~CompactionScheduler() {
    Shutdown();
}

bool CompactionScheduler::ScheduleCompaction(ScheduledCompactionTask task) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shutdown_) {
            return false;
        }
        task_queue_.push(std::move(task));
    }
    cv_task_.notify_one();
    return true;
}

void CompactionScheduler::WaitForCompletion() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_idle_.wait(lock, [this]() {
        return task_queue_.empty() && active_workers_ == 0;
    });
}

void CompactionScheduler::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shutdown_) return;
        shutdown_ = true;
    }
    cv_task_.notify_all();
    cv_idle_.notify_all();

    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    workers_.clear();
}

size_t CompactionScheduler::PendingTasksCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return task_queue_.size();
}

size_t CompactionScheduler::ActiveWorkersCount() const {
    return active_workers_.load(std::memory_order_relaxed);
}

size_t CompactionScheduler::CompletedTasksCount() const {
    return completed_tasks_.load(std::memory_order_relaxed);
}

void CompactionScheduler::WorkerLoop() {
    while (true) {
        ScheduledCompactionTask task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_task_.wait(lock, [this]() {
                return shutdown_ || !task_queue_.empty();
            });

            if (shutdown_ && task_queue_.empty()) {
                return;
            }

            task = std::move(const_cast<ScheduledCompactionTask&>(task_queue_.top()));
            task_queue_.pop();
            active_workers_++;
        }

        // Collect all input file IDs for reservation
        std::vector<uint64_t> file_ids;
        for (const auto& f : task.job.inputs_level) {
            file_ids.push_back(f.file_number);
        }
        for (const auto& f : task.job.inputs_next_level) {
            file_ids.push_back(f.file_number);
        }

        bool locked = true;
        if (concurrency_gate_ && !file_ids.empty()) {
            locked = concurrency_gate_->TryLockFiles(file_ids);
        }

        Status status = Status::OK();
        if (locked) {
            CompactionExecutorOptions exec_opts;
            exec_opts.is_bottom_level = (task.job.level >= 5); // or bottom level indicator
            CompactionExecutor executor(exec_opts, waf_tracker_);

            if (task.next_file_number) {
                status = executor.Execute(task.db_dir, task.next_file_number, &task.job);
            } else {
                uint64_t fallback_num = 1000;
                status = executor.Execute(task.db_dir, &fallback_num, &task.job);
            }

            if (concurrency_gate_ && !file_ids.empty()) {
                concurrency_gate_->UnlockFiles(file_ids);
            }
        } else {
            status = Status::Busy("Compaction candidate files are currently locked by another worker");
        }

        if (task.on_complete) {
            task.on_complete(status, task.job);
        }

        active_workers_--;
        completed_tasks_++;

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (task_queue_.empty() && active_workers_ == 0) {
                cv_idle_.notify_all();
            }
        }
    }
}

} // namespace lsm
