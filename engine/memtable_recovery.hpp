#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "status.hpp"
#include "slice.hpp"
#include "types.hpp"
#include "memtable_queue.hpp"
#include "write_batch.hpp"

namespace lsm {

struct MultiWalRecoveryStats {
    size_t total_wal_files_scanned{0};
    size_t total_records_replayed{0};
    size_t total_batches_replayed{0};
    size_t total_bytes_recovered{0};
    uint64_t last_recovered_seq{0};
    size_t memtable_switches{0};
};

// -----------------------------------------------------------------------------
// MemTableRecoveryCoordinator: Multi-WAL Segment Replay Coordinator
// -----------------------------------------------------------------------------
class MemTableRecoveryCoordinator {
public:
    explicit MemTableRecoveryCoordinator(std::string db_dir);

    // Replays all WAL segments with log number >= min_log_number in strictly ascending order
    Status Recover(uint64_t min_log_number, MemTableQueue* queue, MultiWalRecoveryStats* stats);

private:
    std::string db_dir_;
};

} // namespace lsm
