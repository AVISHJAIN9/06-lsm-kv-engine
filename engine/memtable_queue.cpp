#include "memtable_queue.hpp"

namespace lsm {

MemTableQueue::MemTableQueue(size_t write_buffer_size)
    : write_buffer_size_(write_buffer_size),
      active_(std::make_shared<MemTableCore>()) {}

void MemTableQueue::Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    if (active_) {
        active_->Add(seq, type, key, value);
    }
}

bool MemTableQueue::Get(const Slice& user_key, SequenceNumber snapshot_seq, std::string* value, Status* s) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    // 1. Search active memtable first
    if (active_) {
        Status active_s;
        bool found = active_->Get(user_key, snapshot_seq, value, &active_s);
        if (found) {
            if (s) *s = active_s;
            return true;
        }
    }

    // 2. Search immutable queue from newest to oldest
    for (auto it = imm_queue_.rbegin(); it != imm_queue_.rend(); ++it) {
        if (*it) {
            Status imm_s;
            bool found = (*it)->Get(user_key, snapshot_seq, value, &imm_s);
            if (found) {
                if (s) *s = imm_s;
                return true;
            }
        }
    }

    if (s) *s = Status::NotFound("Key not found in active or immutable memtables");
    return false;
}

Status MemTableQueue::SwitchMemtable(uint64_t new_log_number, std::shared_ptr<MemTableCore>* out_imm) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    if (!active_) {
        active_ = std::make_shared<MemTableCore>();
        active_log_number_.store(new_log_number, std::memory_order_relaxed);
        return Status::OK();
    }

    // Mark current active as immutable and push to queue
    active_->MarkImmutable();
    active_->SetFlushPending(true);

    imm_queue_.push_back(active_);
    imm_count_.store(imm_queue_.size(), std::memory_order_release);

    if (out_imm) {
        *out_imm = active_;
    }

    // Allocate fresh active memtable
    active_ = std::make_shared<MemTableCore>();
    active_log_number_.store(new_log_number, std::memory_order_relaxed);

    return Status::OK();
}

std::shared_ptr<MemTableCore> MemTableQueue::GetActive() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return active_;
}

std::vector<std::shared_ptr<MemTableCore>> MemTableQueue::GetImmutableQueue() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<std::shared_ptr<MemTableCore>> result(imm_queue_.begin(), imm_queue_.end());
    return result;
}

size_t MemTableQueue::ImmQueueSize() const {
    return imm_count_.load(std::memory_order_relaxed);
}

Status MemTableQueue::PopOldestImmutable(std::shared_ptr<MemTableCore>* out_popped) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    if (imm_queue_.empty()) {
        return Status::NotFound("No immutable memtables to pop");
    }

    if (out_popped) {
        *out_popped = imm_queue_.front();
    }
    imm_queue_.pop_front();
    imm_count_.store(imm_queue_.size(), std::memory_order_release);

    return Status::OK();
}

size_t MemTableQueue::TotalMemoryUsage() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    size_t total = 0;
    if (active_) {
        total += active_->ApproximateMemoryUsage();
    }
    for (const auto& imm : imm_queue_) {
        if (imm) {
            total += imm->ApproximateMemoryUsage();
        }
    }
    return total;
}

bool MemTableQueue::IsActiveFlushPending() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!active_) return false;
    return active_->ApproximateMemoryUsage() >= write_buffer_size_;
}

} // namespace lsm
