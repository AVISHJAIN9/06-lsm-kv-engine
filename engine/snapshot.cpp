#include "snapshot.hpp"

namespace lsm {

SnapshotList::SnapshotList() {
    head_.prev_ = &head_;
    head_.next_ = &head_;
    count_ = 0;
}

SnapshotList::~SnapshotList() {
    std::lock_guard<std::mutex> lock(mutex_);
    SnapshotImpl* cur = head_.next_;
    while (cur != &head_) {
        SnapshotImpl* next = cur->next_;
        delete cur;
        cur = next;
    }
    head_.prev_ = &head_;
    head_.next_ = &head_;
    count_ = 0;
}

const SnapshotImpl* SnapshotList::New(SequenceNumber seq) {
    auto* s = new SnapshotImpl(seq);
    std::lock_guard<std::mutex> lock(mutex_);
    // Insert at tail (before head_)
    s->next_ = &head_;
    s->prev_ = head_.prev_;
    s->prev_->next_ = s;
    head_.prev_ = s;
    count_++;
    return s;
}

void SnapshotList::Delete(const SnapshotImpl* snapshot) {
    if (!snapshot || snapshot == &head_) return;

    auto* s = const_cast<SnapshotImpl*>(snapshot);
    std::lock_guard<std::mutex> lock(mutex_);
    s->prev_->next_ = s->next_;
    s->next_->prev_ = s->prev_;
    count_--;
    delete s;
}

} // namespace lsm
