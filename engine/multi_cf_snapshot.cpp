#include "multi_cf_snapshot.hpp"

namespace lsm {

MultiCFSnapshotManager::MultiCFSnapshotManager() {
    head_.next_ = &head_;
    head_.prev_ = &head_;
}

MultiCFSnapshotManager::~MultiCFSnapshotManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (head_.next_ != &head_) {
        MultiCFSnapshot* s = head_.next_;
        s->prev_->next_ = s->next_;
        s->next_->prev_ = s->prev_;
        delete s;
    }
}

const MultiCFSnapshot* MultiCFSnapshotManager::CaptureSnapshot(SequenceNumber current_seq) {
    auto* s = new MultiCFSnapshot(current_seq);
    std::lock_guard<std::mutex> lock(mutex_);
    s->next_ = &head_;
    s->prev_ = head_.prev_;
    s->prev_->next_ = s;
    head_.prev_ = s;
    count_++;
    return s;
}

void MultiCFSnapshotManager::ReleaseSnapshot(const MultiCFSnapshot* snapshot) {
    if (!snapshot) return;

    auto* s = const_cast<MultiCFSnapshot*>(snapshot);
    std::lock_guard<std::mutex> lock(mutex_);
    s->prev_->next_ = s->next_;
    s->next_->prev_ = s->prev_;
    delete s;
    if (count_ > 0) count_--;
}

SequenceNumber MultiCFSnapshotManager::GetOldestSnapshotSeq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (head_.next_ == &head_) {
        return kMaxSequenceNumber;
    }
    return head_.next_->sequence_number_;
}

size_t MultiCFSnapshotManager::ActiveCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

Status MultiCFConsistentReader::Get(
    ColumnFamilyData* cfd,
    const MultiCFSnapshot* snapshot,
    const Slice& key,
    std::string* value
) {
    if (!cfd) {
        return Status::InvalidArgument("Null column family pointer");
    }
    if (cfd->IsDropped()) {
        return Status::InvalidArgument("Column family dropped: " + cfd->GetName());
    }

    SequenceNumber seq = snapshot ? snapshot->GetSequenceNumber() : kMaxSequenceNumber;
    return cfd->Get(key, seq, value);
}

std::vector<std::pair<Status, std::string>> MultiCFConsistentReader::MultiGet(
    ColumnFamilySet* cf_set,
    const MultiCFSnapshot* snapshot,
    const std::vector<std::pair<uint32_t, std::string>>& queries
) {
    std::vector<std::pair<Status, std::string>> results;
    results.reserve(queries.size());

    if (!cf_set) {
        for (size_t i = 0; i < queries.size(); ++i) {
            results.push_back({Status::InvalidArgument("Null ColumnFamilySet"), ""});
        }
        return results;
    }

    SequenceNumber snap_seq = snapshot ? snapshot->GetSequenceNumber() : kMaxSequenceNumber;

    for (const auto& [cf_id, key_str] : queries) {
        auto handle = cf_set->GetColumnFamily(cf_id);
        if (!handle || !handle->cfd()) {
            results.push_back({Status::NotFound("Column family not found: ID=" + std::to_string(cf_id)), ""});
            continue;
        }

        auto* cfd = handle->cfd();
        if (cfd->IsDropped()) {
            results.push_back({Status::InvalidArgument("Column family dropped: " + cfd->GetName()), ""});
            continue;
        }

        std::string val;
        Status s = cfd->Get(Slice(key_str), snap_seq, &val);
        results.push_back({s, std::move(val)});
    }

    return results;
}

} // namespace lsm
