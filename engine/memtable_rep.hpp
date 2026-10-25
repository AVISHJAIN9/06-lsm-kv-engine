#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "iterator.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// MemTableRep: Pluggable abstract interface for in-memory table representations
// Supporting SkipList, Sorted Vector, Prefix-Hash, etc.
// -----------------------------------------------------------------------------
class MemTableRep {
public:
    virtual ~MemTableRep() = default;

    // Adds a mutation (Put / Delete) to the in-memory representation
    virtual void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) = 0;

    // Point lookup for key at or before snapshot_seq
    virtual bool Get(const Slice& user_key, SequenceNumber snapshot_seq, std::string* value, Status* s) const = 0;

    // Fast existence check
    virtual bool Contains(const Slice& user_key) const = 0;

    // Creates an ordered bidirectional iterator over entries
    virtual std::unique_ptr<Iterator> NewIterator() = 0;

    // Memory accounting
    virtual size_t ApproximateMemoryUsage() const = 0;
    virtual size_t Count() const = 0;

    // Immutability and flush lifecycle
    virtual void MarkImmutable() = 0;
    virtual bool IsImmutable() const = 0;
    virtual void SetFlushPending(bool pending) = 0;
    virtual bool IsFlushPending() const = 0;
};

// -----------------------------------------------------------------------------
// MemTableRepFactory: Factory for instantiating pluggable MemTable representations
// -----------------------------------------------------------------------------
class MemTableRepFactory {
public:
    virtual ~MemTableRepFactory() = default;
    virtual std::shared_ptr<MemTableRep> CreateMemTableRep() = 0;
    virtual const char* Name() const = 0;
};

} // namespace lsm
