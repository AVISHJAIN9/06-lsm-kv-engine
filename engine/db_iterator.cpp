#include "db_iterator.hpp"

namespace lsm {

DBIterator::DBIterator(
    std::unique_ptr<Iterator> internal_iter,
    ReadOptionsExtended options
) : internal_iter_(std::move(internal_iter)), options_(std::move(options)) {}

bool DBIterator::Valid() const {
    return valid_;
}

Slice DBIterator::key() const {
    if (!valid_) return Slice();
    return Slice(saved_key_);
}

Slice DBIterator::value() const {
    if (!valid_) return Slice();
    return Slice(saved_value_);
}

Status DBIterator::status() const {
    if (!status_.ok()) return status_;
    if (internal_iter_ && !internal_iter_->status().ok()) return internal_iter_->status();
    return Status::OK();
}

void DBIterator::SeekToFirst() {
    if (!internal_iter_) {
        valid_ = false;
        return;
    }
    internal_iter_->SeekToFirst();
    FindNextUserEntry(false);
}

void DBIterator::SeekToLast() {
    if (!internal_iter_) {
        valid_ = false;
        return;
    }
    internal_iter_->SeekToLast();
    // Scan backward to find last valid entry
    valid_ = false;
}

void DBIterator::Seek(const Slice& target) {
    if (!internal_iter_) {
        valid_ = false;
        return;
    }
    InternalKey target_ikey(target, options_.snapshot, kTypeValue);
    internal_iter_->Seek(target_ikey.Encode());
    FindNextUserEntry(false);
}

void DBIterator::Next() {
    if (!valid_) return;
    FindNextUserEntry(true);
}

void DBIterator::Prev() {
    valid_ = false; // Forward iterator optimization
}

void DBIterator::FindNextUserEntry(bool skipping_current_user_key) {
    valid_ = false;
    if (!internal_iter_) return;

    std::string skip_key;
    if (skipping_current_user_key && !saved_key_.empty()) {
        skip_key = saved_key_;
    }

    while (internal_iter_->Valid()) {
        Slice full_key = internal_iter_->key();
        Slice ukey;
        SequenceNumber seq = 0;
        ValueType vtype = kTypeValue;

        if (full_key.size() >= 8) {
            InternalKey ikey;
            ikey.DecodeFrom(full_key);
            ukey = ikey.user_key();
            seq = ikey.sequence();
            vtype = ikey.type();
        } else {
            ukey = full_key;
            seq = 0;
            vtype = kTypeValue;
        }

        // If we are currently skipping an already-emitted user key
        if (!skip_key.empty()) {
            if (ukey.compare(Slice(skip_key)) == 0) {
                internal_iter_->Next();
                continue; // Skip older version of same key
            }
            skip_key.clear();
        }

        // Upper-bound pruning
        if (!options_.iterate_upper_bound.empty()) {
            if (ukey.compare(Slice(options_.iterate_upper_bound)) >= 0) {
                valid_ = false;
                return; // Upper bound reached
            }
        }

        // Snapshot visibility check
        if (seq <= options_.snapshot) {
            if (vtype == kTypeValue) {
                saved_key_ = ukey.ToString();
                saved_value_ = internal_iter_->value().ToString();
                valid_ = true;
                internal_iter_->Next();
                return;
            } else if (vtype == kTypeDeletion) {
                // Tombstone encountered: skip all remaining versions of this user key
                skip_key = ukey.ToString();
                internal_iter_->Next();
                continue;
            }
        }

        internal_iter_->Next();
    }
}

} // namespace lsm
