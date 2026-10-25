#pragma once

#include <memory>
#include <vector>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "column_family.hpp"
#include "cf_write_batch.hpp"
#include "sequence_generator.hpp"

namespace lsm {

// CFWritePipeline: Coordinates multi-CF atomic write batch ingestion (Feature 100).
// Demuxes operations across independent per-CF MemTables with per-CF arena allocations
// and contiguous sequence number assignment.
class CFWritePipeline {
public:
    CFWritePipeline() = default;
    ~CFWritePipeline() = default;

    // Ingests CFWriteBatch atomically:
    // 1. Reserves contiguous sequence number range for batch.Count() entries.
    // 2. Pre-validates atomic write barrier (e.g. simulated WAL failure).
    // 3. Demuxes and inserts operations into respective target ColumnFamilyData memtables.
    static Status Write(
        CFWriteBatch* batch,
        ColumnFamilySet* cf_set,
        SequenceGenerator* seq_gen,
        bool simulate_wal_failure = false
    );
};

} // namespace lsm
