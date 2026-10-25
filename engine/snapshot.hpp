#pragma once

#include <cstdint>
#include <mutex>
#include "types.hpp"

namespace lsm {

class Snapshot {
public:
    virtual ~Snapshot() = default;
    virtual SequenceNumber GetSequenceNumber() const = 0;
    SequenceNumber sequence_number() const { return GetSequenceNumber(); }
};

class SnapshotImpl : public Snapshot {
public:
    explicit SnapshotImpl(SequenceNumber seq) : sequence_number_(seq) {}
    ~SnapshotImpl() override = default;

    SequenceNumber GetSequenceNumber() const override { return sequence_number_; }

private:
    friend class SnapshotList;
    SequenceNumber sequence_number_{0};
    SnapshotImpl* prev_{nullptr};
    SnapshotImpl* next_{nullptr};
};

class SnapshotList {
public:
    SnapshotList();
    ~SnapshotList();

    SnapshotList(const SnapshotList&) = delete;
    SnapshotList& operator=(const SnapshotList&) = delete;

    const SnapshotImpl* New(SequenceNumber seq);
    void Delete(const SnapshotImpl* snapshot);

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return head_.next_ == &head_;
    }

    SequenceNumber oldest() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (head_.next_ == &head_) ? 0 : head_.next_->sequence_number_;
    }

    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

private:
    mutable std::mutex mutex_{};
    SnapshotImpl head_{0}; // Sentinel circular doubly-linked node
    size_t count_{0};
};

} // namespace lsm
