#include "vector_memtable.hpp"
#include <algorithm>

namespace lsm {

namespace {

struct EntryComparator {
    bool operator()(const VectorMemTableRep::Entry& a, const VectorMemTableRep::Entry& b) const {
        int r = a.user_key.compare(b.user_key);
        if (r != 0) {
            return r < 0;
        }
        // Descending sequence number
        return a.seq > b.seq;
    }
};

} // namespace

class VectorMemTableIterator : public Iterator {
public:
    explicit VectorMemTableIterator(std::vector<VectorMemTableRep::Entry> entries)
        : entries_(std::move(entries)), index_(0) {}

    bool Valid() const override {
        return index_ < entries_.size();
    }

    void SeekToFirst() override {
        index_ = 0;
    }

    void SeekToLast() override {
        if (!entries_.empty()) {
            index_ = entries_.size() - 1;
        } else {
            index_ = 0;
        }
    }

    void Seek(const Slice& target) override {
        VectorMemTableRep::Entry target_entry;
        target_entry.user_key = target.ToString();
        target_entry.seq = kMaxSequenceNumber;

        auto it = std::lower_bound(entries_.begin(), entries_.end(), target_entry, EntryComparator{});
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
        return Slice(entries_[index_].user_key);
    }

    Slice value() const override {
        if (!Valid()) return Slice();
        return Slice(entries_[index_].value);
    }

    Status status() const override {
        return status_;
    }

private:
    std::vector<VectorMemTableRep::Entry> entries_;
    size_t index_{0};
    Status status_{Status::OK()};
};

VectorMemTableRep::VectorMemTableRep() = default;

void VectorMemTableRep::Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
    std::lock_guard<std::mutex> lock(mu_);

    Entry e;
    e.seq = seq;
    e.type = type;
    e.user_key = key.ToString();
    e.value = value.ToString();

    size_t entry_bytes = e.MemoryUsage();
    entries_.push_back(std::move(e));
    is_sorted_ = false;

    memory_usage_.fetch_add(entry_bytes, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
}

void VectorMemTableRep::EnsureSorted() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!is_sorted_) {
        std::stable_sort(entries_.begin(), entries_.end(), EntryComparator{});
        is_sorted_ = true;
    }
}

void VectorMemTableRep::Sort() {
    EnsureSorted();
}

bool VectorMemTableRep::Get(const Slice& user_key, SequenceNumber snapshot_seq, std::string* value, Status* s) const {
    EnsureSorted();

    std::lock_guard<std::mutex> lock(mu_);

    Entry target;
    target.user_key = user_key.ToString();
    target.seq = kMaxSequenceNumber;

    auto it = std::lower_bound(entries_.begin(), entries_.end(), target, EntryComparator{});
    std::string ukey = user_key.ToString();

    while (it != entries_.end() && it->user_key == ukey) {
        if (it->seq <= snapshot_seq) {
            if (it->type == ValueType::kTypeValue) {
                if (value) *value = it->value;
                if (s) *s = Status::OK();
                return true;
            } else if (it->type == ValueType::kTypeDeletion) {
                if (s) *s = Status::NotFound("Key is marked as deleted");
                return false;
            }
        }
        ++it;
    }

    if (s) *s = Status::NotFound("Key not found in VectorMemTable");
    return false;
}

bool VectorMemTableRep::Contains(const Slice& user_key) const {
    std::string val;
    Status s;
    return Get(user_key, kMaxSequenceNumber, &val, &s);
}

std::unique_ptr<Iterator> VectorMemTableRep::NewIterator() {
    EnsureSorted();
    std::lock_guard<std::mutex> lock(mu_);
    return std::make_unique<VectorMemTableIterator>(entries_);
}

} // namespace lsm
