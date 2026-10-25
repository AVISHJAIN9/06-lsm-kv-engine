#include "memtable.hpp"
#include <mutex>
#include <vector>

namespace lsm {

class MemTableIterator : public Iterator {
public:
    explicit MemTableIterator(MemTable* memtable)
        : memtable_(memtable) {
        std::shared_lock<std::shared_mutex> lock(memtable_->mutex_);
        // Snapshot the entries for consistent lock-free iteration
        for (const auto& [k, v] : memtable_->table_) {
            entries_.push_back({k, v});
        }
        index_ = 0;
    }

    ~MemTableIterator() override = default;

    bool Valid() const override {
        return index_ < entries_.size();
    }

    void SeekToFirst() override {
        index_ = 0;
    }

    void SeekToLast() override {
        if (entries_.empty()) {
            index_ = 0;
        } else {
            index_ = entries_.size() - 1;
        }
    }

    void Seek(const Slice& target) override {
        // target is an InternalKey or user key
        InternalKey lookup_key(target, kMaxSequenceNumber, kTypeValue);
        Slice target_slice = lookup_key.Encode();

        auto it = std::lower_bound(
            entries_.begin(), entries_.end(), target_slice,
            [](const MemTableEntry& entry, const Slice& target_val) {
                return InternalKeyComparator(Slice(entry.internal_key), target_val) < 0;
            }
        );
        index_ = std::distance(entries_.begin(), it);
    }

    void Next() override {
        if (Valid()) {
            index_++;
        }
    }

    void Prev() override {
        if (index_ > 0) {
            index_--;
        } else {
            index_ = entries_.size(); // Invalidate
        }
    }

    Slice key() const override {
        if (!Valid()) return Slice();
        // Return User Key part
        Slice internal_slice(entries_[index_].internal_key);
        if (internal_slice.size() >= 8) {
            return Slice(internal_slice.data(), internal_slice.size() - 8);
        }
        return internal_slice;
    }

    Slice value() const override {
        if (!Valid()) return Slice();
        return Slice(entries_[index_].value);
    }

    Status status() const override {
        return Status::OK();
    }

private:
    MemTable* memtable_{nullptr};
    std::vector<MemTableEntry> entries_{};
    size_t index_{0};
};

MemTable::MemTable() {
    memory_usage_ = sizeof(MemTable);
}

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
    InternalKey ikey(key, seq, type);
    std::string ikey_str = ikey.Encode().ToString();
    std::string val_str = value.ToString();

    std::unique_lock<std::shared_mutex> lock(mutex_);
    memory_usage_ += ikey_str.size() + val_str.size() + sizeof(void*) * 4;
    table_[std::move(ikey_str)] = std::move(val_str);
}

bool MemTable::Get(const Slice& user_key, SequenceNumber snapshot_seq, std::string* value, Status* s) {
    InternalKey lookup_key(user_key, snapshot_seq, kTypeValue);
    std::string lookup_str = lookup_key.Encode().ToString();

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = table_.lower_bound(lookup_str);

    if (it != table_.end()) {
        Slice found_internal_key(it->first);
        if (found_internal_key.size() >= 8) {
            Slice found_user_key(found_internal_key.data(), found_internal_key.size() - 8);
            if (found_user_key == user_key) {
                uint64_t tag = 0;
                const uint8_t* ptr = reinterpret_cast<const uint8_t*>(
                    found_internal_key.data() + found_internal_key.size() - 8
                );
                for (int i = 0; i < 8; ++i) {
                    tag |= (static_cast<uint64_t>(ptr[i]) << (i * 8));
                }

                ValueType type = ExtractValueType(tag);
                if (type == ValueType::kTypeValue) {
                    if (value) *value = it->second;
                    *s = Status::OK();
                    return true;
                } else if (type == ValueType::kTypeDeletion) {
                    *s = Status::NotFound("Deleted (Tombstone)");
                    return true;
                }
            }
        }
    }

    *s = Status::NotFound("Key not found");
    return false;
}

bool MemTable::HasKeyBeenModifiedSince(const Slice& user_key, SequenceNumber seq) const {
    InternalKey lookup_key(user_key, kMaxSequenceNumber, kTypeValue);
    std::string lookup_str = lookup_key.Encode().ToString();

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = table_.lower_bound(lookup_str);

    while (it != table_.end()) {
        Slice found_internal_key(it->first);
        if (found_internal_key.size() >= 8) {
            Slice found_user_key(found_internal_key.data(), found_internal_key.size() - 8);
            if (found_user_key == user_key) {
                uint64_t tag = 0;
                const uint8_t* ptr = reinterpret_cast<const uint8_t*>(
                    found_internal_key.data() + found_internal_key.size() - 8
                );
                for (int i = 0; i < 8; ++i) {
                    tag |= (static_cast<uint64_t>(ptr[i]) << (i * 8));
                }
                SequenceNumber entry_seq = ExtractSequenceNumber(tag);
                if (entry_seq > seq) {
                    return true;
                }
            } else {
                break;
            }
        }
        it++;
    }
    return false;
}

std::unique_ptr<Iterator> MemTable::NewIterator() {
    return std::make_unique<MemTableIterator>(this);
}

size_t MemTable::ApproximateMemoryUsage() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return memory_usage_;
}

size_t MemTable::Count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return table_.size();
}

} // namespace lsm
