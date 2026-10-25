#pragma once

#include <string>
#include <vector>
#include <memory>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "write_batch.hpp"
#include "sequence_generator.hpp"
#include "memtable_core.hpp"

namespace lsm {

// AtomicBatchCommitter: Coordinates two-phase atomic write barriers guaranteeing
// strict all-or-nothing isolation and contiguous sequence window assignment (Feature 87).
class AtomicBatchCommitter {
public:
    AtomicBatchCommitter() = default;
    ~AtomicBatchCommitter() = default;

    // Commits write batch atomically:
    // 1. Reserves a contiguous window of sequence numbers [start_seq, start_seq + count - 1].
    // 2. Pre-validates atomic write barrier (e.g. WAL sync simulated failure).
    // 3. Atomically applies operations to the active MemTable.
    // If failure occurs before Step 3, rolls back and preserves engine consistency.
    static Status Commit(
        WriteBatch* batch,
        SequenceGenerator* seq_gen,
        MemTableCore* memtable,
        bool simulate_wal_failure = false
    );
};

} // namespace lsm
