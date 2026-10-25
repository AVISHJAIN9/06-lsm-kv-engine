#pragma once

#include "memtable_rep.hpp"
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <memory>

namespace lsm {

// -----------------------------------------------------------------------------
// VectorMemTableRep: High-performance sorted vector implementation
// Optimized for bulk-loading and read-heavy workloads
// -----------------------------------------------------------------------------
class VectorMemTableRep : public MemTableRep {
public:
    struct Entry {
        SequenceNumber seq{0};
        ValueType type{ValueType::kTypeValue};
        std::string user_key;
        std::string value;

        size_t MemoryUsage() const {
            return sizeof(Entry) + user_key.capacity() + value.capacity();
        }
    };

    VectorMemTableRep();
    ~VectorMemTableRep() override = default;

    VectorMemTableRep(const VectorMemTableRep&) = delete;
    VectorMemTableRep& operator=(const VectorMemTableRep&) = delete;

    void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) override;
    bool Get(const Slice& user_key, SequenceNumber snapshot_seq, std::string* value, Status* s) const override;
    bool Contains(const Slice& user_key) const override;
    std::unique_ptr<Iterator> NewIterator() override;

    size_t ApproximateMemoryUsage() const override {
        return memory_usage_.load(std::memory_order_relaxed);
    }

    size_t Count() const override {
        return count_.load(std::memory_order_relaxed);
    }

    void MarkImmutable() override { is_immutable_.store(true, std::memory_order_release); }
    bool IsImmutable() const override { return is_immutable_.load(std::memory_order_acquire); }

    void SetFlushPending(bool pending) override { is_flush_pending_.store(pending, std::memory_order_relaxed); }
    bool IsFlushPending() const override { return is_flush_pending_.load(std::memory_order_relaxed); }

    void Sort();

private:
    friend class VectorMemTableIterator;

    void EnsureSorted() const;

    mutable std::mutex mu_;
    mutable std::vector<Entry> entries_;
    mutable bool is_sorted_{true};

    std::atomic<size_t> memory_usage_{0};
    std::atomic<size_t> count_{0};
    std::atomic<bool> is_immutable_{false};
    std::atomic<bool> is_flush_pending_{false};
};

// -----------------------------------------------------------------------------
// VectorMemTableRepFactory
// -----------------------------------------------------------------------------
class VectorMemTableRepFactory : public MemTableRepFactory {
public:
    std::shared_ptr<MemTableRep> CreateMemTableRep() override {
        return std::make_shared<VectorMemTableRep>();
    }

    const char* Name() const override {
        return "VectorMemTableRepFactory";
    }
};

} // namespace lsm
