#pragma once

#include <cstdint>
#include <string>
#include "slice.hpp"
#include "types.hpp"
#include "snapshot_manager.hpp"

namespace lsm {

// TombstonePurger: Snapshot-aware deletion tombstone lifetime and compaction purge evaluator (Feature 85).
// Retains deletion markers if visible to active snapshots or needed to mask keys in deeper levels.
class TombstonePurger {
public:
    TombstonePurger() = default;
    ~TombstonePurger() = default;

    // Evaluates whether a deletion tombstone should be preserved in the compacted output SSTable.
    // - key: target user key
    // - tomb_seq: sequence number of the deletion marker
    // - current_level: compaction target level
    // - key_exists_in_deeper_levels: true if older versions of this key exist in levels > current_level
    // - oldest_active_snapshot: lowest sequence number among all living snapshots
    static bool ShouldRetainTombstone(
        const Slice& key,
        SequenceNumber tomb_seq,
        int current_level,
        bool key_exists_in_deeper_levels,
        SequenceNumber oldest_active_snapshot
    );

    // Helper checking if tombstone is visible to an active snapshot
    static bool IsVisibleToSnapshot(SequenceNumber tomb_seq, SequenceNumber oldest_active_snapshot) {
        // If tombstone sequence is newer than or equal to oldest active snapshot,
        // it must be preserved to service reads at that snapshot.
        return tomb_seq >= oldest_active_snapshot;
    }
};

} // namespace lsm
