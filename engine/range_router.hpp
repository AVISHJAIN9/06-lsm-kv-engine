#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include "slice.hpp"
#include "status.hpp"

namespace lsm {

using PartitionId = uint64_t;
using NodeId = uint64_t;

// RangeDescriptor: Metadata defining a contiguous half-open key interval [start_key, end_key)
struct RangeDescriptor {
    PartitionId partition_id{0};
    std::string start_key;  // Inclusive (empty string means -infinity)
    std::string end_key;    // Exclusive (empty string means +infinity)
    NodeId leader_node_id{0};
    uint64_t epoch{1};

    bool Contains(const Slice& key) const {
        std::string k = key.ToString();
        if (!start_key.empty() && k < start_key) {
            return false;
        }
        if (!end_key.empty() && k >= end_key) {
            return false;
        }
        return true;
    }
};

// RangeRouteTable: Thread-safe dynamic directory mapping keys to partitions
class RangeRouteTable {
public:
    RangeRouteTable() = default;
    ~RangeRouteTable() = default;

    RangeRouteTable(const RangeRouteTable&) = delete;
    RangeRouteTable& operator=(const RangeRouteTable&) = delete;

    // Registers or updates a partition range
    Status UpdatePartition(const RangeDescriptor& desc);

    // Finds the partition descriptor hosting the target key
    Status FindPartition(const Slice& key, RangeDescriptor* out_desc) const;

    // Finds a partition by ID
    Status GetPartitionById(PartitionId pid, RangeDescriptor* out_desc) const;

    // Splits an existing partition into two child partitions at split_key
    Status SplitRange(
        PartitionId old_partition_id,
        const std::string& split_key,
        PartitionId new_left_id,
        PartitionId new_right_id,
        NodeId left_leader,
        NodeId right_leader
    );

    // Merges two adjacent partitions
    Status MergeRanges(
        PartitionId left_id,
        PartitionId right_id,
        PartitionId merged_id,
        NodeId merged_leader
    );

    // Returns all active partition descriptors
    std::vector<RangeDescriptor> GetAllPartitions() const;

    // Returns total count of active partitions
    size_t Size() const;

    // Clears the routing table
    void Clear();

private:
    mutable std::shared_mutex mutex_;
    // Map indexed by start_key
    std::map<std::string, RangeDescriptor> ranges_by_start_;
    // Map indexed by partition_id
    std::map<PartitionId, RangeDescriptor> ranges_by_id_;
};

} // namespace lsm
