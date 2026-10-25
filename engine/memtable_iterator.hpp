#pragma once

#include <vector>
#include <string>
#include <memory>
#include "iterator.hpp"
#include "memtable.hpp"
#include "types.hpp"
#include "ttl_util.hpp"

namespace lsm {

struct UserEntry {
    std::string user_key;
    std::string user_value;
};

class UserMemTableIterator : public Iterator {
public:
    UserMemTableIterator(MemTable* memtable, SequenceNumber snapshot_seq);
    ~UserMemTableIterator() override = default;

    bool Valid() const override;
    void SeekToFirst() override;
    void SeekToLast() override;
    void Seek(const Slice& target) override;
    void Next() override;
    void Prev() override;
    Slice key() const override;
    Slice value() const override;
    Status status() const override;

private:
    void BuildSnapshotEntries(MemTable* memtable, SequenceNumber snapshot_seq);

    std::vector<UserEntry> entries_{};
    size_t index_{0};
};

} // namespace lsm
