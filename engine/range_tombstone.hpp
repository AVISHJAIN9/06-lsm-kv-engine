#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include "slice.hpp"
#include "types.hpp"

namespace lsm {

// RangeTombstone: Represents a contiguous half-open interval deletion [start_key, end_key)
// committed at a specific sequence number (Feature 86).
struct RangeTombstone {
    std::string start_key{};
    std::string end_key{};
    SequenceNumber seq{0};

    RangeTombstone() = default;
    RangeTombstone(std::string start, std::string end, SequenceNumber s)
        : start_key(std::move(start)), end_key(std::move(end)), seq(s) {}

    bool Contains(const Slice& user_key) const {
        return (user_key.compare(Slice(start_key)) >= 0 &&
                user_key.compare(Slice(end_key)) < 0);
    }
};

// RangeDelAggregator: Collects and indexes active range tombstones across memtables
// and multi-level SSTables to perform O(log N) point key masking and range pruning.
class RangeDelAggregator {
public:
    RangeDelAggregator() = default;
    ~RangeDelAggregator() = default;

    // Adds a range deletion marker [start_key, end_key) at sequence number 'seq'.
    void AddTombstone(const RangeTombstone& tomb);
    void AddTombstone(const Slice& start_key, const Slice& end_key, SequenceNumber seq);

    // Evaluates whether user_key at key_seq is masked by any active range tombstone.
    // Returns true if there exists a range tombstone containing user_key with tomb.seq > key_seq.
    bool ShouldDelete(const Slice& user_key, SequenceNumber key_seq) const;

    // Evaluates whether user_key is covered by a range tombstone at or before snapshot_seq.
    bool ShouldDeleteAtSnapshot(const Slice& user_key, SequenceNumber key_seq, SequenceNumber snapshot_seq) const;

    size_t Count() const;
    bool IsEmpty() const;
    void Clear();

    const std::vector<RangeTombstone>& GetAllTombstones() const { return tombstones_; }

private:
    std::vector<RangeTombstone> tombstones_{};
};

} // namespace lsm
