#pragma once

#include <memory>
#include <string>
#include "iterator.hpp"
#include "slice.hpp"
#include "status.hpp"

namespace lsm {

struct RangeIteratorOptions {
    std::string lower_bound{}; // Inclusive
    std::string upper_bound{}; // Exclusive
    size_t max_keys{0};        // 0 = unlimited
    size_t max_bytes{0};       // 0 = unlimited
};

// -----------------------------------------------------------------------------
// RangeIterator: Bounded [lower_bound, upper_bound) Interval Range Iterator
// Supports max key count limits, byte budgets, and early termination
// -----------------------------------------------------------------------------
class RangeIterator : public Iterator {
public:
    RangeIterator(
        std::unique_ptr<Iterator> iter,
        RangeIteratorOptions options
    );
    ~RangeIterator() override = default;

    RangeIterator(const RangeIterator&) = delete;
    RangeIterator& operator=(const RangeIterator&) = delete;

    bool Valid() const override;
    void SeekToFirst() override;
    void SeekToLast() override;
    void Seek(const Slice& target) override;
    void Next() override;
    void Prev() override;
    Slice key() const override;
    Slice value() const override;
    Status status() const override;

    size_t KeysYielded() const { return keys_yielded_; }
    size_t BytesYielded() const { return bytes_yielded_; }

private:
    void CheckBounds();

    std::unique_ptr<Iterator> iter_;
    RangeIteratorOptions options_;

    bool valid_{false};
    size_t keys_yielded_{0};
    size_t bytes_yielded_{0};
};

} // namespace lsm
