#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <atomic>
#include <vector>
#include "slice.hpp"
#include "status.hpp"
#include "range_router.hpp"
#include "memtable_core.hpp"
#include "txn_participant.hpp"
#include "wound_wait.hpp"

namespace lsm {

// Partition: Encapsulates a live partition shard hosting a key range and local LSM engine
struct Partition {
    RangeDescriptor descriptor;
    std::shared_ptr<MemTableCore> memtable;
    std::shared_ptr<TxnParticipant> participant;
    std::atomic<size_t> approximate_bytes{0};
    std::atomic<bool> is_frozen{false};

    Partition(
        RangeDescriptor desc,
        std::shared_ptr<WoundWaitEngine> lock_engine = nullptr
    ) : descriptor(std::move(desc)),
        memtable(std::make_shared<MemTableCore>()) {
        participant = std::make_shared<TxnParticipant>(descriptor.partition_id, memtable, lock_engine);
    }

    Status Put(const std::string& key, const std::string& value, SequenceNumber seq = 1) {
        if (is_frozen.load(std::memory_order_relaxed)) {
            return Status::Busy("Partition is currently frozen for split transition");
        }
        if (!descriptor.Contains(Slice(key))) {
            return Status::InvalidArgument("Key out of partition range");
        }
        memtable->Add(seq, ValueType::kTypeValue, Slice(key), Slice(value));
        approximate_bytes.fetch_add(key.size() + value.size() + 16, std::memory_order_relaxed);
        return Status::OK();
    }

    Status Get(const Slice& key, SequenceNumber seq, std::string* value) const {
        if (!descriptor.Contains(key)) {
            return Status::InvalidArgument("Key out of partition range");
        }
        Status s;
        bool found = memtable->Get(key, seq, value, &s);
        if (!found) {
            return Status::NotFound("Key not found in partition");
        }
        return s;
    }
};

// RangeSplitter: Automates partition boundary evaluation and atomic cutover splitting (Feature 132)
class RangeSplitter {
public:
    RangeSplitter() = default;
    ~RangeSplitter() = default;

    // Checks if partition size exceeds max threshold triggering split
    static bool CheckSplitNeeded(const Partition& partition, size_t max_bytes_threshold);

    // Executes partition split at split_key and updates route directory
    static Status ExecuteSplit(
        std::shared_ptr<Partition> parent,
        const std::string& split_key,
        PartitionId new_right_id,
        NodeId right_leader,
        RangeRouteTable* route_table,
        std::shared_ptr<WoundWaitEngine> lock_engine,
        std::shared_ptr<Partition>& out_left,
        std::shared_ptr<Partition>& out_right
    );
};

} // namespace lsm
