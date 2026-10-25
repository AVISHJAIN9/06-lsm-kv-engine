#pragma once

#include <memory>
#include <string>
#include "iterator.hpp"
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"

namespace lsm {

struct ReadOptionsExtended {
    SequenceNumber snapshot{kMaxSequenceNumber};
    std::string iterate_upper_bound{}; // Optional upper bound prefix/slice
    bool verify_checksums{false};
    bool fill_cache{true};
};

// -----------------------------------------------------------------------------
// DBIterator: User-Facing Linear Forward Iterator with Boundary & Tombstone Pruning
// -----------------------------------------------------------------------------
class DBIterator : public Iterator {
public:
    DBIterator(
        std::unique_ptr<Iterator> internal_iter,
        ReadOptionsExtended options
    );
    ~DBIterator() override = default;

    DBIterator(const DBIterator&) = delete;
    DBIterator& operator=(const DBIterator&) = delete;

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
    void FindNextUserEntry(bool skipping_current_user_key = false);

    std::unique_ptr<Iterator> internal_iter_;
    ReadOptionsExtended options_;

    bool valid_{false};
    std::string saved_key_;
    std::string saved_value_;
    Status status_{Status::OK()};
};

} // namespace lsm
