#include "range_router.hpp"

namespace lsm {

Status RangeRouteTable::UpdatePartition(const RangeDescriptor& desc) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ranges_by_start_[desc.start_key] = desc;
    ranges_by_id_[desc.partition_id] = desc;
    return Status::OK();
}

Status RangeRouteTable::FindPartition(const Slice& key, RangeDescriptor* out_desc) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (ranges_by_start_.empty()) {
        return Status::NotFound("RangeRouteTable is empty");
    }

    std::string k = key.ToString();
    auto it = ranges_by_start_.upper_bound(k);
    if (it != ranges_by_start_.begin()) {
        --it;
        if (it->second.Contains(key)) {
            if (out_desc) *out_desc = it->second;
            return Status::OK();
        }
    }

    // Secondary scan across all registered ranges in case of custom configurations
    for (const auto& kv : ranges_by_id_) {
        if (kv.second.Contains(key)) {
            if (out_desc) *out_desc = kv.second;
            return Status::OK();
        }
    }

    return Status::NotFound("No matching partition range found for key: " + k);
}

Status RangeRouteTable::GetPartitionById(PartitionId pid, RangeDescriptor* out_desc) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = ranges_by_id_.find(pid);
    if (it == ranges_by_id_.end()) {
        return Status::NotFound("Partition ID not found: " + std::to_string(pid));
    }
    if (out_desc) *out_desc = it->second;
    return Status::OK();
}

Status RangeRouteTable::SplitRange(
    PartitionId old_partition_id,
    const std::string& split_key,
    PartitionId new_left_id,
    PartitionId new_right_id,
    NodeId left_leader,
    NodeId right_leader
) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = ranges_by_id_.find(old_partition_id);
    if (it == ranges_by_id_.end()) {
        return Status::NotFound("Parent partition ID not found for split: " + std::to_string(old_partition_id));
    }

    RangeDescriptor parent = it->second;
    if (!parent.start_key.empty() && split_key <= parent.start_key) {
        return Status::InvalidArgument("Split key must be strictly greater than parent start_key");
    }
    if (!parent.end_key.empty() && split_key >= parent.end_key) {
        return Status::InvalidArgument("Split key must be strictly less than parent end_key");
    }

    // Remove old partition
    ranges_by_start_.erase(parent.start_key);
    ranges_by_id_.erase(old_partition_id);

    // Add new left partition [parent.start_key, split_key)
    RangeDescriptor left_desc;
    left_desc.partition_id = new_left_id;
    left_desc.start_key = parent.start_key;
    left_desc.end_key = split_key;
    left_desc.leader_node_id = left_leader;
    left_desc.epoch = parent.epoch + 1;

    // Add new right partition [split_key, parent.end_key)
    RangeDescriptor right_desc;
    right_desc.partition_id = new_right_id;
    right_desc.start_key = split_key;
    right_desc.end_key = parent.end_key;
    right_desc.leader_node_id = right_leader;
    right_desc.epoch = parent.epoch + 1;

    ranges_by_start_[left_desc.start_key] = left_desc;
    ranges_by_id_[left_desc.partition_id] = left_desc;

    ranges_by_start_[right_desc.start_key] = right_desc;
    ranges_by_id_[right_desc.partition_id] = right_desc;

    return Status::OK();
}

Status RangeRouteTable::MergeRanges(
    PartitionId left_id,
    PartitionId right_id,
    PartitionId merged_id,
    NodeId merged_leader
) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it_left = ranges_by_id_.find(left_id);
    auto it_right = ranges_by_id_.find(right_id);
    if (it_left == ranges_by_id_.end() || it_right == ranges_by_id_.end()) {
        return Status::NotFound("Partitions to merge not found");
    }

    RangeDescriptor left = it_left->second;
    RangeDescriptor right = it_right->second;

    if (left.end_key != right.start_key) {
        return Status::InvalidArgument("Partitions are not adjacent: left.end_key != right.start_key");
    }

    ranges_by_start_.erase(left.start_key);
    ranges_by_start_.erase(right.start_key);
    ranges_by_id_.erase(left_id);
    ranges_by_id_.erase(right_id);

    RangeDescriptor merged;
    merged.partition_id = merged_id;
    merged.start_key = left.start_key;
    merged.end_key = right.end_key;
    merged.leader_node_id = merged_leader;
    merged.epoch = std::max(left.epoch, right.epoch) + 1;

    ranges_by_start_[merged.start_key] = merged;
    ranges_by_id_[merged.partition_id] = merged;

    return Status::OK();
}

std::vector<RangeDescriptor> RangeRouteTable::GetAllPartitions() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<RangeDescriptor> result;
    result.reserve(ranges_by_start_.size());
    for (const auto& kv : ranges_by_start_) {
        result.push_back(kv.second);
    }
    return result;
}

size_t RangeRouteTable::Size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return ranges_by_id_.size();
}

void RangeRouteTable::Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ranges_by_start_.clear();
    ranges_by_id_.clear();
}

} // namespace lsm
