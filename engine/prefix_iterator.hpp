#pragma once

#include <memory>
#include <string>
#include "iterator.hpp"
#include "slice.hpp"
#include "status.hpp"

namespace lsm {

class PrefixIterator : public Iterator {
public:
    PrefixIterator(std::unique_ptr<Iterator> iter, const Slice& prefix);
    ~PrefixIterator() override = default;

    bool Valid() const override;
    void SeekToFirst() override;
    void SeekToLast() override;
    void Seek(const Slice& target) override;
    void Next() override;
    void Prev() override;
    Slice key() const override;
    Slice value() const override;
    Status status() const override;

    static std::string CalculateUpperBound(const Slice& prefix);

private:
    std::unique_ptr<Iterator> iter_{nullptr};
    std::string prefix_{};
    std::string upper_bound_{};
    bool has_upper_bound_{false};

    void CheckPrefixBound();
};

} // namespace lsm
