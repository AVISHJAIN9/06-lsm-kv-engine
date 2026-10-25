#pragma once

#include <string>
#include <vector>
#include <memory>
#include "slice.hpp"
#include "status.hpp"
#include "format.hpp"
#include "block_builder.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// IndexBuilder: Builds single-level and two-level partitioned sparse index blocks
// -----------------------------------------------------------------------------
class IndexBuilder {
public:
    explicit IndexBuilder(int restart_interval = 1);
    ~IndexBuilder() = default;

    IndexBuilder(const IndexBuilder&) = delete;
    IndexBuilder& operator=(const IndexBuilder&) = delete;

    // Resets the builder state
    void Reset();

    // Adds an index entry mapping a separator key to a data block handle
    void AddIndexEntry(
        std::string* last_key_in_current_block,
        const Slice* first_key_in_next_block,
        const BlockHandle& handle
    );

    // Finalizes the index block and returns the encoded slice
    Slice Finish();

    // Returns current size estimate in bytes
    size_t CurrentSizeEstimate() const {
        return block_builder_.CurrentSizeEstimate();
    }

    bool Empty() const {
        return block_builder_.Empty();
    }

    size_t NumEntries() const {
        return num_entries_;
    }

private:
    BlockBuilder block_builder_;
    std::string last_key_;
    size_t num_entries_{0};
};

// -----------------------------------------------------------------------------
// TwoLevelIndexBuilder: Two-level partitioned index builder for large SSTables
// -----------------------------------------------------------------------------
class TwoLevelIndexBuilder {
public:
    explicit TwoLevelIndexBuilder(size_t max_partition_size = 2048, int restart_interval = 1);

    void AddIndexEntry(
        std::string* last_key_in_current_block,
        const Slice* first_key_in_next_block,
        const BlockHandle& handle,
        std::vector<std::string>* out_completed_partition_blocks
    );

    // Finishes any in-flight partition and returns top-level index block slice
    Slice Finish(std::vector<std::string>* out_completed_partition_blocks);

    bool IsTwoLevel() const { return total_partitions_ > 1 || !top_level_builder_.Empty(); }
    int restart_interval() const { return restart_interval_; }

private:
    void FlushCurrentPartition(std::vector<std::string>* out_completed_partition_blocks);

    size_t max_partition_size_{2048};
    int restart_interval_{1};
    BlockBuilder current_partition_builder_;
    BlockBuilder top_level_builder_;
    std::string last_partition_key_;
    size_t total_partitions_{0};
    uint64_t simulated_partition_offset_{0};
};

} // namespace lsm
