#include "range_tombstone.hpp"

namespace lsm {

void RangeDelAggregator::AddTombstone(const RangeTombstone& tomb) {
    if (tomb.start_key >= tomb.end_key) {
        return; // Empty interval
    }
    tombstones_.push_back(tomb);
}

void RangeDelAggregator::AddTombstone(const Slice& start_key, const Slice& end_key, SequenceNumber seq) {
    if (start_key.compare(end_key) >= 0) {
        return;
    }
    tombstones_.emplace_back(start_key.ToString(), end_key.ToString(), seq);
}

bool RangeDelAggregator::ShouldDelete(const Slice& user_key, SequenceNumber key_seq) const {
    for (const auto& tomb : tombstones_) {
        if (tomb.Contains(user_key) && tomb.seq > key_seq) {
            return true;
        }
    }
    return false;
}

bool RangeDelAggregator::ShouldDeleteAtSnapshot(
    const Slice& user_key,
    SequenceNumber key_seq,
    SequenceNumber snapshot_seq
) const {
    for (const auto& tomb : tombstones_) {
        // Tombstone must be visible to snapshot and newer than point key version
        if (tomb.seq <= snapshot_seq && tomb.seq > key_seq && tomb.Contains(user_key)) {
            return true;
        }
    }
    return false;
}

size_t RangeDelAggregator::Count() const {
    return tombstones_.size();
}

bool RangeDelAggregator::IsEmpty() const {
    return tombstones_.empty();
}

void RangeDelAggregator::Clear() {
    tombstones_.clear();
}

} // namespace lsm
