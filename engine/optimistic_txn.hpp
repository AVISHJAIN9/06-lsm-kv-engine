#pragma once

#include <string>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "write_batch.hpp"
#include "snapshot_manager.hpp"
#include "sequence_generator.hpp"
#include "memtable_core.hpp"
#include "conflict_detector.hpp"
#include "atomic_batch.hpp"

namespace lsm {

// OptimisticTxnContext: ACID optimistic transaction client interface with
// read-your-own-writes, snapshot isolation, and commit-time validation (Feature 88).
class OptimisticTxnContext {
public:
    OptimisticTxnContext(
        SnapshotManager* snap_mgr,
        SequenceGenerator* seq_gen,
        MemTableCore* memtable
    );
    ~OptimisticTxnContext();

    OptimisticTxnContext(const OptimisticTxnContext&) = delete;
    OptimisticTxnContext& operator=(const OptimisticTxnContext&) = delete;

    // Reads key respecting snapshot isolation and local uncommitted writes (Read-Your-Own-Writes).
    Status Get(const Slice& key, std::string* value);

    // Reads key, adds to read-set for conflict detection, and respects local writes.
    Status GetForUpdate(const Slice& key, std::string* value);

    // Buffers put mutation into transaction private write buffer.
    Status Put(const Slice& key, const Slice& value);

    // Buffers delete mutation into transaction private write buffer.
    Status Delete(const Slice& key);

    // Validates read/write conflict sets, allocates sequence numbers, and atomically commits.
    Status Commit();

    // Reverts all buffered uncommitted mutations and releases snapshot.
    Status Rollback();

    SequenceNumber GetSnapshotSeq() const { return snapshot_seq_; }
    size_t PendingWriteCount() const { return write_map_.size(); }
    bool IsCommitted() const { return committed_; }
    bool IsRolledBack() const { return rolled_back_; }

private:
    SnapshotManager* snap_mgr_{nullptr};
    SequenceGenerator* seq_gen_{nullptr};
    MemTableCore* memtable_{nullptr};

    const Snapshot* snapshot_handle_{nullptr};
    SequenceNumber snapshot_seq_{0};

    std::unordered_set<std::string> read_set_{};
    std::unordered_map<std::string, std::pair<ValueType, std::string>> write_map_{};
    WriteBatch write_batch_{};

    bool committed_{false};
    bool rolled_back_{false};
};

} // namespace lsm
