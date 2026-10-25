#include "memtable_iterator.hpp"
#include <algorithm>

namespace lsm {

UserMemTableIterator::UserMemTableIterator(MemTable* memtable, SequenceNumber snapshot_seq) {
    BuildSnapshotEntries(memtable, snapshot_seq);
    index_ = 0;
}

void UserMemTableIterator::BuildSnapshotEntries(MemTable* memtable, SequenceNumber snapshot_seq) {
    entries_.clear();
    if (!memtable) return;

    auto raw_iter = memtable->NewIterator();
    std::string last_saved_user_key = "";
    bool has_saved_key = false;

    for (raw_iter->SeekToFirst(); raw_iter->Valid(); raw_iter->Next()) {
        Slice ukey = raw_iter->key();
        std::string ukey_str = ukey.ToString();

        // If we already evaluated this user key, skip older versions
        if (has_saved_key && ukey_str == last_saved_user_key) {
            continue;
        }

        // Query point lookup in memtable respecting snapshot_seq
        std::string val;
        Status s;
        if (memtable->Get(ukey, snapshot_seq, &val, &s)) {
            if (s.ok()) {
                // Check TTL expiration
                if (!TTLUtil::IsExpired(Slice(val))) {
                    Slice stripped_val;
                    uint64_t exp = 0;
                    TTLUtil::DecodeTTL(Slice(val), &stripped_val, &exp);
                    entries_.push_back({ukey_str, stripped_val.ToString()});
                }
            }
            // If tombstone or expired, it won't be pushed
        }

        last_saved_user_key = ukey_str;
        has_saved_key = true;
    }
}

bool UserMemTableIterator::Valid() const {
    return index_ < entries_.size();
}

void UserMemTableIterator::SeekToFirst() {
    index_ = 0;
}

void UserMemTableIterator::SeekToLast() {
    if (entries_.empty()) {
        index_ = 0;
    } else {
        index_ = entries_.size() - 1;
    }
}

void UserMemTableIterator::Seek(const Slice& target) {
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), target,
        [](const UserEntry& entry, const Slice& target_slice) {
            return Slice(entry.user_key).compare(target_slice) < 0;
        }
    );
    index_ = std::distance(entries_.begin(), it);
}

void UserMemTableIterator::Next() {
    if (Valid()) {
        index_++;
    }
}

void UserMemTableIterator::Prev() {
    if (index_ > 0) {
        index_--;
    } else {
        index_ = entries_.size(); // Invalidate
    }
}

Slice UserMemTableIterator::key() const {
    if (!Valid()) return Slice();
    return Slice(entries_[index_].user_key);
}

Slice UserMemTableIterator::value() const {
    if (!Valid()) return Slice();
    return Slice(entries_[index_].user_value);
}

Status UserMemTableIterator::status() const {
    return Status::OK();
}

} // namespace lsm
