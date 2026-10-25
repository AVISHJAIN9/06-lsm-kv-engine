#pragma once

#include <cstdint>
#include <mutex>
#include <vector>
#include <chrono>
#include "types.hpp"
#include "snapshot.hpp"

namespace lsm {

// SnapshotNode: Intrusive doubly-linked list node tracking sequence and allocation timestamp
class SnapshotNode : public Snapshot {
public:
    SnapshotNode(SequenceNumber seq, uint64_t creation_time_ns)
        : sequence_number_(seq), creation_time_ns_(creation_time_ns) {}
    ~SnapshotNode() override = default;

    SequenceNumber GetSequenceNumber() const override { return sequence_number_; }
    uint64_t GetCreationTimeNs() const { return creation_time_ns_; }

private:
    friend class SnapshotManager;
    SequenceNumber sequence_number_{0};
    uint64_t creation_time_ns_{0};
    SnapshotNode* prev_{nullptr};
    SnapshotNode* next_{nullptr};
};

// SnapshotManager: Thread-safe registry and lifecycle manager for point-in-time snapshots
// with expiration tracking, TTL sweep reclamation, and oldest-LSN compaction unblocking.
class SnapshotManager {
public:
    SnapshotManager();
    ~SnapshotManager();

    SnapshotManager(const SnapshotManager&) = delete;
    SnapshotManager& operator=(const SnapshotManager&) = delete;

    // Creates a new snapshot pinned to 'seq' with current timestamp.
    const Snapshot* CreateSnapshot(SequenceNumber seq);

    // Releases and deallocates a snapshot handle.
    void ReleaseSnapshot(const Snapshot* snapshot);

    // Returns the oldest active snapshot sequence, or 'fallback_seq' if no snapshots are active.
    SequenceNumber GetOldestSnapshot(SequenceNumber fallback_seq = 0) const;

    // Checks active snapshots and invalidates/frees those older than max_age_ns.
    // Returns the number of swept expired snapshots.
    size_t SweepExpiredSnapshots(uint64_t max_age_ns, std::vector<SequenceNumber>* out_expired_seqs = nullptr);

    // Returns number of active snapshots.
    size_t Count() const;

    // Returns true if no snapshots are currently registered.
    bool IsEmpty() const;

    // Returns all active snapshot sequence numbers in chronological order.
    std::vector<SequenceNumber> GetAllSnapshots() const;

    // Static helper to get current monotonic time in nanoseconds.
    static uint64_t NowNs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }

private:
    mutable std::mutex mutex_{};
    SnapshotNode head_{0, 0}; // Sentinel circular node
    size_t count_{0};
};

} // namespace lsm
