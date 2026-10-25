#include "tombstone_purger.hpp"

namespace lsm {

bool TombstonePurger::ShouldRetainTombstone(
    const Slice& /*key*/,
    SequenceNumber tomb_seq,
    int /*current_level*/,
    bool key_exists_in_deeper_levels,
    SequenceNumber oldest_active_snapshot
) {
    // Rule 1: If an active snapshot exists that was created at or before this tombstone,
    // the tombstone must be retained so queries at that snapshot observe the deletion.
    if (oldest_active_snapshot != 0 && tomb_seq >= oldest_active_snapshot) {
        return true;
    }

    // Rule 2: If there are older historical versions of this key residing in deeper LSM levels,
    // the tombstone must be retained to shadow/mask those obsolete versions during future reads.
    if (key_exists_in_deeper_levels) {
        return true;
    }

    // Rule 3: If tomb_seq < oldest_active_snapshot (or no active snapshots exist)
    // and no versions exist in deeper levels (e.g. bottommost level), the tombstone can be safely purged.
    return false;
}

} // namespace lsm
