#pragma once

#include <string>
#include <vector>
#include <memory>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "memtable_queue.hpp"
#include "leveled_compaction.hpp"
#include "read_amplification.hpp"
#include "table_reader.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// PointLookupEngine: Hierarchical short-circuiting point query cascade
// Order: Active MemTable -> ImmQueue (Newest->Oldest) -> L0 Files -> L1..Lmax Files
// -----------------------------------------------------------------------------
class PointLookupEngine {
public:
    PointLookupEngine(
        std::shared_ptr<MemTableQueue> memtable_queue,
        std::vector<std::vector<FileMetaData>> levels,
        TableOptions table_options = {},
        std::shared_ptr<BlockCache> block_cache = nullptr,
        std::shared_ptr<ReadAmplificationTracker> raf_tracker = nullptr
    );

    // Executes hierarchical point lookup with snapshot sequence visibility
    Status Get(
        const Slice& user_key,
        SequenceNumber snapshot_seq,
        std::string* value,
        int* out_tier_hit = nullptr
    );

    void UpdateLevels(std::vector<std::vector<FileMetaData>> levels) {
        levels_ = std::move(levels);
    }

private:
    std::shared_ptr<MemTableQueue> memtable_queue_;
    std::vector<std::vector<FileMetaData>> levels_;
    TableOptions table_options_;
    std::shared_ptr<BlockCache> block_cache_;
    std::shared_ptr<ReadAmplificationTracker> raf_tracker_;
};

} // namespace lsm
