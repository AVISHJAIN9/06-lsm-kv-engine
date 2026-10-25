#include "merging_iterator.hpp"

namespace lsm {

MergingIterator::MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
    : children_(std::move(children)) {}

bool MergingIterator::Valid() const {
    return current_ != nullptr && current_->Valid();
}

void MergingIterator::FindSmallest() {
    Iterator* smallest = nullptr;
    for (auto& child : children_) {
        if (child->Valid()) {
            if (smallest == nullptr) {
                smallest = child.get();
            } else if (InternalKeyComparator(child->key(), smallest->key()) < 0) {
                smallest = child.get();
            }
        }
    }
    current_ = smallest;
    direction_ = kForward;
}

void MergingIterator::FindLargest() {
    Iterator* largest = nullptr;
    for (auto& child : children_) {
        if (child->Valid()) {
            if (largest == nullptr) {
                largest = child.get();
            } else if (InternalKeyComparator(child->key(), largest->key()) > 0) {
                largest = child.get();
            }
        }
    }
    current_ = largest;
    direction_ = kReverse;
}

void MergingIterator::SeekToFirst() {
    for (auto& child : children_) {
        child->SeekToFirst();
    }
    FindSmallest();
}

void MergingIterator::SeekToLast() {
    for (auto& child : children_) {
        child->SeekToLast();
    }
    FindLargest();
}

void MergingIterator::Seek(const Slice& target) {
    for (auto& child : children_) {
        child->Seek(target);
    }
    FindSmallest();
}

void MergingIterator::Next() {
    if (!Valid()) return;

    if (direction_ != kForward) {
        for (auto& child : children_) {
            if (child.get() != current_) {
                child->Seek(current_->key());
                if (child->Valid() && InternalKeyComparator(child->key(), current_->key()) == 0) {
                    child->Next();
                }
            }
        }
        direction_ = kForward;
    }

    current_->Next();
    FindSmallest();
}

void MergingIterator::Prev() {
    if (!Valid()) return;

    if (direction_ != kReverse) {
        for (auto& child : children_) {
            if (child.get() != current_) {
                child->Seek(current_->key());
                if (child->Valid()) {
                    child->Prev();
                } else {
                    child->SeekToLast();
                }
            }
        }
        direction_ = kReverse;
    }

    current_->Prev();
    FindLargest();
}

Slice MergingIterator::key() const {
    if (!Valid()) return Slice();
    return current_->key();
}

Slice MergingIterator::value() const {
    if (!Valid()) return Slice();
    return current_->value();
}

Status MergingIterator::status() const {
    Status s = Status::OK();
    for (const auto& child : children_) {
        if (!child->status().ok()) {
            s = child->status();
            break;
        }
    }
    return s;
}

} // namespace lsm
