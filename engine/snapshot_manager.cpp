#include "snapshot_manager.hpp"

namespace lsm {

SnapshotManager::SnapshotManager() {
    head_.prev_ = &head_;
    head_.next_ = &head_;
    count_ = 0;
}

SnapshotManager::~SnapshotManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    SnapshotNode* cur = head_.next_;
    while (cur != &head_) {
        SnapshotNode* next = cur->next_;
        delete cur;
        cur = next;
    }
    head_.prev_ = &head_;
    head_.next_ = &head_;
    count_ = 0;
}

const Snapshot* SnapshotManager::CreateSnapshot(SequenceNumber seq) {
    uint64_t now_ns = NowNs();
    auto* s = new SnapshotNode(seq, now_ns);

    std::lock_guard<std::mutex> lock(mutex_);
    // Append to circular doubly-linked list tail (before head_)
    s->next_ = &head_;
    s->prev_ = head_.prev_;
    s->prev_->next_ = s;
    head_.prev_ = s;
    count_++;
    return s;
}

void SnapshotManager::ReleaseSnapshot(const Snapshot* snapshot) {
    if (!snapshot || snapshot == &head_) return;

    auto* s = const_cast<SnapshotNode*>(static_cast<const SnapshotNode*>(snapshot));
    std::lock_guard<std::mutex> lock(mutex_);
    // Verify node is still linked in the list before unlinking
    if (s->prev_ && s->next_) {
        s->prev_->next_ = s->next_;
        s->next_->prev_ = s->prev_;
        s->prev_ = nullptr;
        s->next_ = nullptr;
        count_--;
        delete s;
    }
}

SequenceNumber SnapshotManager::GetOldestSnapshot(SequenceNumber fallback_seq) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (head_.next_ == &head_) {
        return fallback_seq;
    }
    return head_.next_->sequence_number_;
}

size_t SnapshotManager::SweepExpiredSnapshots(uint64_t max_age_ns, std::vector<SequenceNumber>* out_expired_seqs) {
    uint64_t now_ns = NowNs();
    size_t swept = 0;

    std::lock_guard<std::mutex> lock(mutex_);
    SnapshotNode* cur = head_.next_;
    while (cur != &head_) {
        SnapshotNode* next = cur->next_;
        if (now_ns >= cur->creation_time_ns_ && (now_ns - cur->creation_time_ns_) >= max_age_ns) {
            // Unlink expired node
            cur->prev_->next_ = cur->next_;
            cur->next_->prev_ = cur->prev_;
            cur->prev_ = nullptr;
            cur->next_ = nullptr;
            count_--;
            swept++;
            if (out_expired_seqs) {
                out_expired_seqs->push_back(cur->sequence_number_);
            }
            delete cur;
        }
        cur = next;
    }
    return swept;
}

size_t SnapshotManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

bool SnapshotManager::IsEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ == 0;
}

std::vector<SequenceNumber> SnapshotManager::GetAllSnapshots() const {
    std::vector<SequenceNumber> result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(count_);
    SnapshotNode* cur = head_.next_;
    while (cur != &head_) {
        result.push_back(cur->sequence_number_);
        cur = cur->next_;
    }
    return result;
}

} // namespace lsm
