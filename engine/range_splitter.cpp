#include "range_splitter.hpp"
#include "memtable_iter.hpp"

namespace lsm {

bool RangeSplitter::CheckSplitNeeded(const Partition& partition, size_t max_bytes_threshold) {
    if (partition.approximate_bytes.load(std::memory_order_relaxed) >= max_bytes_threshold) {
        return true;
    }
    if (partition.memtable && partition.memtable->ApproximateMemoryUsage() >= max_bytes_threshold) {
        return true;
    }
    return false;
}

Status RangeSplitter::ExecuteSplit(
    std::shared_ptr<Partition> parent,
    const std::string& split_key,
    PartitionId new_right_id,
    NodeId right_leader,
    RangeRouteTable* route_table,
    std::shared_ptr<WoundWaitEngine> lock_engine,
    std::shared_ptr<Partition>& out_left,
    std::shared_ptr<Partition>& out_right
) {
    if (!parent || !route_table) {
        return Status::InvalidArgument("Null parent or route table in ExecuteSplit");
    }

    // Freeze writes on parent partition during migration cutover
    parent->is_frozen.store(true, std::memory_order_release);

    RangeDescriptor parent_desc = parent->descriptor;

    // Create Left Partition [parent.start_key, split_key)
    RangeDescriptor left_desc;
    left_desc.partition_id = parent_desc.partition_id;
    left_desc.start_key = parent_desc.start_key;
    left_desc.end_key = split_key;
    left_desc.leader_node_id = parent_desc.leader_node_id;
    left_desc.epoch = parent_desc.epoch + 1;

    // Create Right Partition [split_key, parent.end_key)
    RangeDescriptor right_desc;
    right_desc.partition_id = new_right_id;
    right_desc.start_key = split_key;
    right_desc.end_key = parent_desc.end_key;
    right_desc.leader_node_id = right_leader;
    right_desc.epoch = parent_desc.epoch + 1;

    out_left = std::make_shared<Partition>(left_desc, lock_engine);
    out_right = std::make_shared<Partition>(right_desc, lock_engine);

    // Migrate entries from parent memtable to child partitions
    if (parent->memtable) {
        std::unique_ptr<Iterator> iter = parent->memtable->NewIterator();
        auto* m_iter = dynamic_cast<MemTableIter*>(iter.get());

        for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
            Slice uk = iter->key();
            Slice val = iter->value();
            SequenceNumber seq = m_iter ? m_iter->sequence() : 1;
            ValueType type = m_iter ? m_iter->type() : ValueType::kTypeValue;

            std::string k_str = uk.ToString();
            if (k_str < split_key) {
                out_left->memtable->Add(seq, type, uk, val);
                out_left->approximate_bytes.fetch_add(uk.size() + val.size() + 16, std::memory_order_relaxed);
            } else {
                out_right->memtable->Add(seq, type, uk, val);
                out_right->approximate_bytes.fetch_add(uk.size() + val.size() + 16, std::memory_order_relaxed);
            }
        }
    }

    // Atomically update route directory
    Status s = route_table->SplitRange(
        parent_desc.partition_id,
        split_key,
        left_desc.partition_id,
        right_desc.partition_id,
        left_desc.leader_node_id,
        right_desc.leader_node_id
    );

    parent->is_frozen.store(false, std::memory_order_release);
    return s;
}

} // namespace lsm
