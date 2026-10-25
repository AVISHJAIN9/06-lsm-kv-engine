#pragma once

#include <map>
#include <shared_mutex>
#include <memory>
#include <string>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "iterator.hpp"

namespace lsm {

struct MemTableEntry {
    std::string internal_key;
    std::string value;
};

struct MemTableKeyComparator {
    bool operator()(const std::string& a, const std::string& b) const {
        return InternalKeyComparator(Slice(a), Slice(b)) < 0;
    }
};

class MemTableIterator;

class MemTable {
public:
    MemTable();
    ~MemTable() = default;

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);
    bool Get(const Slice& user_key, SequenceNumber snapshot_seq, std::string* value, Status* s);
    bool HasKeyBeenModifiedSince(const Slice& user_key, SequenceNumber seq) const;

    std::unique_ptr<Iterator> NewIterator();

    size_t ApproximateMemoryUsage() const;
    size_t Count() const;

private:
    friend class MemTableIterator;

    using Table = std::map<std::string, std::string, MemTableKeyComparator>;

    mutable std::shared_mutex mutex_{};
    Table table_{};
    size_t memory_usage_{0};
};

} // namespace lsm
