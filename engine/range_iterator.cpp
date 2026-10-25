#include "range_iterator.hpp"

namespace lsm {

RangeIterator::RangeIterator(
    std::unique_ptr<Iterator> iter,
    RangeIteratorOptions options
) : iter_(std::move(iter)), options_(std::move(options)) {}

bool RangeIterator::Valid() const {
    return valid_ && iter_ && iter_->Valid();
}

Slice RangeIterator::key() const {
    if (!Valid()) return Slice();
    return iter_->key();
}

Slice RangeIterator::value() const {
    if (!Valid()) return Slice();
    return iter_->value();
}

Status RangeIterator::status() const {
    if (!iter_) return Status::Corruption("Null iterator");
    return iter_->status();
}

void RangeIterator::CheckBounds() {
    if (!iter_ || !iter_->Valid()) {
        valid_ = false;
        return;
    }

    // Lower bound check
    if (!options_.lower_bound.empty()) {
        if (iter_->key().compare(Slice(options_.lower_bound)) < 0) {
            valid_ = false;
            return;
        }
    }

    // Upper bound check (exclusive)
    if (!options_.upper_bound.empty()) {
        if (iter_->key().compare(Slice(options_.upper_bound)) >= 0) {
            valid_ = false;
            return;
        }
    }

    // Key limit check
    if (options_.max_keys > 0 && keys_yielded_ >= options_.max_keys) {
        valid_ = false;
        return;
    }

    // Byte budget check
    if (options_.max_bytes > 0 && bytes_yielded_ >= options_.max_bytes) {
        valid_ = false;
        return;
    }

    valid_ = true;
}

void RangeIterator::SeekToFirst() {
    keys_yielded_ = 0;
    bytes_yielded_ = 0;
    if (!iter_) {
        valid_ = false;
        return;
    }

    if (!options_.lower_bound.empty()) {
        iter_->Seek(Slice(options_.lower_bound));
    } else {
        iter_->SeekToFirst();
    }
    CheckBounds();
}

void RangeIterator::SeekToLast() {
    keys_yielded_ = 0;
    bytes_yielded_ = 0;
    if (!iter_) {
        valid_ = false;
        return;
    }

    if (!options_.upper_bound.empty()) {
        iter_->Seek(Slice(options_.upper_bound));
        if (iter_->Valid()) {
            iter_->Prev();
        } else {
            iter_->SeekToLast();
        }
    } else {
        iter_->SeekToLast();
    }
    CheckBounds();
}

void RangeIterator::Seek(const Slice& target) {
    keys_yielded_ = 0;
    bytes_yielded_ = 0;
    if (!iter_) {
        valid_ = false;
        return;
    }

    if (!options_.lower_bound.empty() && target.compare(Slice(options_.lower_bound)) < 0) {
        iter_->Seek(Slice(options_.lower_bound));
    } else {
        iter_->Seek(target);
    }
    CheckBounds();
}

void RangeIterator::Next() {
    if (!Valid()) return;

    keys_yielded_++;
    bytes_yielded_ += iter_->key().size() + iter_->value().size();

    // Check limits before advancing
    if (options_.max_keys > 0 && keys_yielded_ >= options_.max_keys) {
        valid_ = false;
        return;
    }
    if (options_.max_bytes > 0 && bytes_yielded_ >= options_.max_bytes) {
        valid_ = false;
        return;
    }

    iter_->Next();
    CheckBounds();
}

void RangeIterator::Prev() {
    if (!Valid()) return;
    iter_->Prev();
    CheckBounds();
}

} // namespace lsm
