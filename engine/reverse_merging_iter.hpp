#pragma once

#include <memory>
#include <string>
#include <vector>
#include "iterator.hpp"
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "merging_iterator.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// ReverseMergingIterator: Backward traversal with multi-version deduplication
// Guarantees strict descending order and suppresses shadowed older versions & tombstones
// -----------------------------------------------------------------------------
class ReverseMergingIterator : public Iterator {
public:
    ReverseMergingIterator(
        std::unique_ptr<Iterator> merge_iter,
        SequenceNumber snapshot_seq = kMaxSequenceNumber
    );
    ~ReverseMergingIterator() override = default;

    ReverseMergingIterator(const ReverseMergingIterator&) = delete;
    ReverseMergingIterator& operator=(const ReverseMergingIterator&) = delete;

    bool Valid() const override;
    void SeekToFirst() override;
    void SeekToLast() override;
    void Seek(const Slice& target) override;
    void Next() override;
    void Prev() override;
    Slice key() const override;
    Slice value() const override;
    Status status() const override;

private:
    void FindPrevUserKey(const std::string& current_user_key);

    std::unique_ptr<Iterator> merge_iter_;
    SequenceNumber snapshot_seq_{kMaxSequenceNumber};

    bool valid_{false};
    std::string saved_key_;
    std::string saved_value_;
    Status status_{Status::OK()};
};

} // namespace lsm
