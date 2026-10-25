#pragma once

#include <string>
#include <vector>
#include <memory>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "write_batch.hpp"
#include "memtable_core.hpp"
#include "sequence_generator.hpp"

namespace lsm {

// TxnRecord: Represents a serialized transaction entry in the write-ahead log
struct TxnWALRecord {
    SequenceNumber seq{0};
    bool is_committed{false};
    WriteBatch batch{};
};

// TxnRecoveryManager: Crash recovery coordinator parsing WAL logs, validating commit
// markers, replaying committed mutations, and dropping uncommitted records (Feature 90).
class TxnRecoveryManager {
public:
    TxnRecoveryManager() = default;
    ~TxnRecoveryManager() = default;

    // Replays a sequence of WAL transaction records:
    // - Replays only records with is_committed == true into the memtable.
    // - Updates seq_gen to max committed sequence.
    // - Drops uncommitted or partial transaction batches.
    static Status ReplayTransactions(
        const std::vector<TxnWALRecord>& wal_records,
        MemTableCore* memtable,
        SequenceGenerator* seq_gen,
        size_t* out_committed_replayed = nullptr,
        size_t* out_uncommitted_discarded = nullptr
    );
};

} // namespace lsm
