#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "status.hpp"
#include "types.hpp"
#include "column_family.hpp"
#include "table_builder.hpp"

namespace lsm {

// CFFlushController: Multi-Tenant Flush Scheduling Coordinator (Feature 101).
// Manages independent per-CF MemTable flushes to Level 0 SSTables while coordinating
// a unified global minimum flushed sequence watermark for safe WAL segment truncation.
class CFFlushController {
public:
    CFFlushController() = default;
    ~CFFlushController() = default;

    // Scans all registered CFs and identifies candidate memtables exceeding write_buffer_size
    static std::vector<std::shared_ptr<ColumnFamilyData>> PickMemtablesToFlush(ColumnFamilySet* cf_set);

    // Flushes specific ColumnFamilyData immutable memtables to a new Level 0 SSTable file.
    // Advances CF's last_flushed_seq watermark upon completion.
    static Status FlushColumnFamily(
        const std::string& dbname,
        ColumnFamilyData* cfd,
        uint64_t sstable_file_number,
        SequenceNumber current_max_seq
    );

    // Computes GlobalMinFlushedLSN across all active column families: min(cf.last_flushed_seq)
    static SequenceNumber GetGlobalMinFlushedLSN(ColumnFamilySet* cf_set);

    // Determines if a WAL file covering sequences up to wal_max_seq is safe to unlink.
    // Returns true ONLY when ALL column families have flushed past wal_max_seq.
    static bool IsWALSafeToUnlink(ColumnFamilySet* cf_set, SequenceNumber wal_max_seq);
};

} // namespace lsm
