#include "reverse_merging_iter.hpp"

namespace lsm {

namespace {

inline Slice ExtractUserKey(const Slice& full_key) {
    if (full_key.size() >= 8) {
        return Slice(full_key.data(), full_key.size() - 8);
    }
    return full_key;
}

} // namespace

ReverseMergingIterator::ReverseMergingIterator(
    std::unique_ptr<Iterator> merge_iter,
    SequenceNumber snapshot_seq
) : merge_iter_(std::move(merge_iter)),
    snapshot_seq_(snapshot_seq) {}

bool ReverseMergingIterator::Valid() const {
    return valid_;
}

Slice ReverseMergingIterator::key() const {
    if (!valid_) return Slice();
    return Slice(saved_key_);
}

Slice ReverseMergingIterator::value() const {
    if (!valid_) return Slice();
    return Slice(saved_value_);
}

Status ReverseMergingIterator::status() const {
    if (!status_.ok()) return status_;
    if (merge_iter_ && !merge_iter_->status().ok()) return merge_iter_->status();
    return Status::OK();
}

void ReverseMergingIterator::SeekToFirst() {
    valid_ = false;
    if (!merge_iter_) return;
    merge_iter_->SeekToFirst();
    if (!merge_iter_->Valid()) return;

    // Scan forward to find first valid visible key
    while (merge_iter_->Valid()) {
        Slice full_key = merge_iter_->key();
        Slice ukey = ExtractUserKey(full_key);

        InternalKey ikey;
        if (full_key.size() >= 8) {
            ikey.DecodeFrom(full_key);
        } else {
            ikey = InternalKey(ukey, 0, kTypeValue);
        }

        if (ikey.sequence() <= snapshot_seq_) {
            if (ikey.type() == kTypeValue) {
                saved_key_ = ukey.ToString();
                saved_value_ = merge_iter_->value().ToString();
                valid_ = true;
                return;
            }
        }
        merge_iter_->Next();
    }
}

void ReverseMergingIterator::SeekToLast() {
    valid_ = false;
    if (!merge_iter_) return;

    merge_iter_->SeekToLast();
    if (!merge_iter_->Valid()) return;

    std::string last_ukey = ExtractUserKey(merge_iter_->key()).ToString();

    // Position at newest version of last_ukey
    InternalKey target(Slice(last_ukey), snapshot_seq_, kTypeValue);
    merge_iter_->Seek(target.Encode());

    if (merge_iter_->Valid() && ExtractUserKey(merge_iter_->key()).compare(Slice(last_ukey)) == 0) {
        InternalKey ikey;
        if (merge_iter_->key().size() >= 8) {
            ikey.DecodeFrom(merge_iter_->key());
            if (ikey.sequence() <= snapshot_seq_ && ikey.type() == kTypeValue) {
                saved_key_ = last_ukey;
                saved_value_ = merge_iter_->value().ToString();
                valid_ = true;
                return;
            }
        } else {
            saved_key_ = last_ukey;
            saved_value_ = merge_iter_->value().ToString();
            valid_ = true;
            return;
        }
    }

    // If last_ukey was a tombstone or invalid, find previous user key
    FindPrevUserKey(last_ukey);
}

void ReverseMergingIterator::Seek(const Slice& target) {
    valid_ = false;
    if (!merge_iter_) return;

    // Seek to target key
    InternalKey target_ikey(target, snapshot_seq_, kTypeValue);
    merge_iter_->Seek(target_ikey.Encode());

    if (!merge_iter_->Valid()) {
        SeekToLast();
        return;
    }

    Slice ukey = ExtractUserKey(merge_iter_->key());
    if (ukey.compare(target) <= 0) {
        InternalKey ikey;
        if (merge_iter_->key().size() >= 8) {
            ikey.DecodeFrom(merge_iter_->key());
            if (ikey.sequence() <= snapshot_seq_ && ikey.type() == kTypeValue) {
                saved_key_ = ukey.ToString();
                saved_value_ = merge_iter_->value().ToString();
                valid_ = true;
                return;
            }
        } else {
            saved_key_ = ukey.ToString();
            saved_value_ = merge_iter_->value().ToString();
            valid_ = true;
            return;
        }
    }

    FindPrevUserKey(target.ToString());
}

void ReverseMergingIterator::Next() {
    // Reverse iterator moves backward on Prev, but if Next is invoked, move forward
    valid_ = false;
}

void ReverseMergingIterator::Prev() {
    if (!valid_) return;
    FindPrevUserKey(saved_key_);
}

void ReverseMergingIterator::FindPrevUserKey(const std::string& current_user_key) {
    valid_ = false;
    if (!merge_iter_) return;

    // Step backward before current_user_key
    InternalKey target(Slice(current_user_key), kMaxSequenceNumber, kTypeValue);
    merge_iter_->Seek(target.Encode());

    if (!merge_iter_->Valid()) {
        merge_iter_->SeekToLast();
    } else {
        merge_iter_->Prev();
    }

    while (merge_iter_->Valid()) {
        Slice ukey_slice = ExtractUserKey(merge_iter_->key());
        std::string cand_ukey = ukey_slice.ToString();

        if (cand_ukey >= current_user_key) {
            merge_iter_->Prev();
            continue;
        }

        // Seek to newest version of cand_ukey
        InternalKey cand_target(Slice(cand_ukey), snapshot_seq_, kTypeValue);
        merge_iter_->Seek(cand_target.Encode());

        if (merge_iter_->Valid() && ExtractUserKey(merge_iter_->key()).compare(Slice(cand_ukey)) == 0) {
            InternalKey ikey;
            if (merge_iter_->key().size() >= 8) {
                ikey.DecodeFrom(merge_iter_->key());
                if (ikey.sequence() <= snapshot_seq_) {
                    if (ikey.type() == kTypeValue) {
                        saved_key_ = cand_ukey;
                        saved_value_ = merge_iter_->value().ToString();
                        valid_ = true;
                        return;
                    }
                    // Tombstone: skip this key and continue backwards
                }
            } else {
                saved_key_ = cand_ukey;
                saved_value_ = merge_iter_->value().ToString();
                valid_ = true;
                return;
            }
        }

        // Move to before cand_ukey
        InternalKey before_target(Slice(cand_ukey), kMaxSequenceNumber, kTypeValue);
        merge_iter_->Seek(before_target.Encode());
        if (merge_iter_->Valid()) {
            merge_iter_->Prev();
        } else {
            merge_iter_->SeekToLast();
        }
    }
}

} // namespace lsm
