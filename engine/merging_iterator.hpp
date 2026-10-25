#pragma once

#include <vector>
#include <memory>
#include "iterator.hpp"
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// MergingIterator: Multi-Way Min-Heap Merging Iterator over Heterogeneous Iterators
// Unifies active memtable, immutable queue, L0 files, and L1..Lmax iterators
// -----------------------------------------------------------------------------
class MergingIterator : public Iterator {
public:
    explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children);
    ~MergingIterator() override = default;

    MergingIterator(const MergingIterator&) = delete;
    MergingIterator& operator=(const MergingIterator&) = delete;

    bool Valid() const override;
    void SeekToFirst() override;
    void SeekToLast() override;
    void Seek(const Slice& target) override;
    void Next() override;
    void Prev() override;
    Slice key() const override;
    Slice value() const override;
    Status status() const override;

    size_t NumChildren() const { return children_.size(); }

private:
    void FindSmallest();
    void FindLargest();

    std::vector<std::unique_ptr<Iterator>> children_;
    Iterator* current_{nullptr};
    enum Direction { kForward, kReverse };
    Direction direction_{kForward};
};

} // namespace lsm
