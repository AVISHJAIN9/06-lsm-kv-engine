#include "prefix_iterator.hpp"

namespace lsm {

std::string PrefixIterator::CalculateUpperBound(const Slice& prefix) {
    std::string bound = prefix.ToString();
    for (int i = static_cast<int>(bound.size()) - 1; i >= 0; --i) {
        if (static_cast<uint8_t>(bound[i]) < 0xFF) {
            bound[i] = static_cast<char>(static_cast<uint8_t>(bound[i]) + 1);
            bound.resize(i + 1);
            return bound;
        }
    }
    return ""; // All 0xFF, no upper bound
}

PrefixIterator::PrefixIterator(std::unique_ptr<Iterator> iter, const Slice& prefix)
    : iter_(std::move(iter)), prefix_(prefix.ToString()) {
    upper_bound_ = CalculateUpperBound(prefix);
    has_upper_bound_ = !upper_bound_.empty();
}

void PrefixIterator::CheckPrefixBound() {
    // If not valid or doesn't start with prefix, invalid
}

bool PrefixIterator::Valid() const {
    if (!iter_ || !iter_->Valid()) return false;
    return iter_->key().starts_with(prefix_);
}

void PrefixIterator::SeekToFirst() {
    if (!iter_) return;
    iter_->Seek(prefix_);
}

void PrefixIterator::SeekToLast() {
    if (!iter_) return;
    if (has_upper_bound_) {
        iter_->Seek(upper_bound_);
        if (iter_->Valid()) {
            iter_->Prev();
        } else {
            iter_->SeekToLast();
        }
    } else {
        iter_->SeekToLast();
    }
}

void PrefixIterator::Seek(const Slice& target) {
    if (!iter_) return;
    if (target.starts_with(prefix_)) {
        iter_->Seek(target);
    } else if (target < prefix_) {
        iter_->Seek(prefix_);
    } else {
        // Target is past prefix
        if (has_upper_bound_) {
            iter_->Seek(upper_bound_);
        } else {
            iter_->SeekToLast();
            iter_->Next(); // Invalidate
        }
    }
}

void PrefixIterator::Next() {
    if (iter_ && iter_->Valid()) {
        iter_->Next();
    }
}

void PrefixIterator::Prev() {
    if (iter_ && iter_->Valid()) {
        iter_->Prev();
    }
}

Slice PrefixIterator::key() const {
    if (!Valid()) return Slice();
    return iter_->key();
}

Slice PrefixIterator::value() const {
    if (!Valid()) return Slice();
    return iter_->value();
}

Status PrefixIterator::status() const {
    if (!iter_) return Status::Corruption("Null iterator");
    return iter_->status();
}

} // namespace lsm
