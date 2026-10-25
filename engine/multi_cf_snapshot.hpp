#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <memory>
#include "types.hpp"
#include "slice.hpp"
#include "status.hpp"
#include "column_family.hpp"

namespace lsm {

// MultiCFSnapshot: Represents a globally consistent point-in-time snapshot
// pinning state across all active column families at sequence number S_global (Feature 105).
class MultiCFSnapshot {
public:
    explicit MultiCFSnapshot(SequenceNumber seq) : sequence_number_(seq) {}
    ~MultiCFSnapshot() = default;

    SequenceNumber GetSequenceNumber() const { return sequence_number_; }

private:
    friend class MultiCFSnapshotManager;
    SequenceNumber sequence_number_{0};
    MultiCFSnapshot* prev_{nullptr};
    MultiCFSnapshot* next_{nullptr};
};

// MultiCFSnapshotManager: Manages the lifecycle of atomic multi-CF snapshots (Feature 105)
class MultiCFSnapshotManager {
public:
    MultiCFSnapshotManager();
    ~MultiCFSnapshotManager();

    MultiCFSnapshotManager(const MultiCFSnapshotManager&) = delete;
    MultiCFSnapshotManager& operator=(const MultiCFSnapshotManager&) = delete;

    // Captures an atomic snapshot at the current global sequence number (<= 30 ns acquisition latency)
    const MultiCFSnapshot* CaptureSnapshot(SequenceNumber current_seq);

    // Releases an acquired snapshot
    void ReleaseSnapshot(const MultiCFSnapshot* snapshot);

    // Returns the sequence number of the oldest active snapshot (or kMaxSequenceNumber if none)
    SequenceNumber GetOldestSnapshotSeq() const;

    size_t ActiveCount() const;

private:
    mutable std::mutex mutex_{};
    MultiCFSnapshot head_{0}; // Sentinel circular doubly linked node
    size_t count_{0};
};

// MultiCFConsistentReader: Executes multi-table atomic queries honoring the snapshot boundary
class MultiCFConsistentReader {
public:
    static Status Get(
        ColumnFamilyData* cfd,
        const MultiCFSnapshot* snapshot,
        const Slice& key,
        std::string* value
    );

    static std::vector<std::pair<Status, std::string>> MultiGet(
        ColumnFamilySet* cf_set,
        const MultiCFSnapshot* snapshot,
        const std::vector<std::pair<uint32_t, std::string>>& queries
    );
};

} // namespace lsm
