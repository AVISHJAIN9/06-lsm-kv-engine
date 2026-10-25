#pragma once

#include <string>
#include <atomic>
#include <memory>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "arena.hpp"
#include "skiplist.hpp"
#include "iterator.hpp"

namespace lsm {

class MemTableIterator;

class MemTableCore {
public:
    struct KeyComparator {
        int operator()(const char* a, const char* b) const;
    };

    using Table = SkipList<const char*, KeyComparator>;

    MemTableCore();
    ~MemTableCore() = default;

    MemTableCore(const MemTableCore&) = delete;
    MemTableCore& operator=(const MemTableCore&) = delete;

    // Adds a record (Value or Deletion tombstone) with sequence number to the memtable
    void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

    // Looks up a key at or before target snapshot sequence number
    bool Get(const Slice& user_key, SequenceNumber snapshot_seq, std::string* value, Status* s);

    // Returns an iterator over the memtable entries
    std::unique_ptr<Iterator> NewIterator();

    // Memory accounting
    size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

    // Flush threshold state
    bool IsFlushPending() const { return is_flush_pending_.load(std::memory_order_relaxed); }
    void SetFlushPending(bool pending) { is_flush_pending_.store(pending, std::memory_order_relaxed); }

    void MarkImmutable() { is_immutable_.store(true, std::memory_order_release); }
    bool IsImmutable() const { return is_immutable_.load(std::memory_order_acquire); }

    size_t Count() const { return num_entries_.load(std::memory_order_relaxed); }

private:
    friend class MemTableIterator;

    KeyComparator comparator_{};
    Arena arena_{};
    Table table_;

    std::atomic<bool> is_flush_pending_{false};
    std::atomic<bool> is_immutable_{false};
    std::atomic<size_t> num_entries_{0};

    // Helper functions for entry encoding and decoding
    static const char* EncodeEntry(
        Arena* arena,
        SequenceNumber seq,
        ValueType type,
        const Slice& key,
        const Slice& value
    );

    static Slice GetLengthPrefixedSlice(const char* data);
};

} // namespace lsm
