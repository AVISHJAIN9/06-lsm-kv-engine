#pragma once

#include <memory>
#include <mutex>
#include <functional>
#include <atomic>
#include "status.hpp"
#include "slice.hpp"
#include "raft_node.hpp"
#include "raft_applier.hpp"
#include "memtable_core.hpp"

namespace lsm {

// ReadIndexCoordinator: High-throughput linearizable Read-Index and leader lease coordinator (Feature 125).
// Eliminates stale reads on partitioned former leaders without disk writes (<= 15 us lease read latency).
class ReadIndexCoordinator {
public:
    explicit ReadIndexCoordinator(std::shared_ptr<RaftNode> raft_node);
    ~ReadIndexCoordinator() = default;

    ReadIndexCoordinator(const ReadIndexCoordinator&) = delete;
    ReadIndexCoordinator& operator=(const ReadIndexCoordinator&) = delete;

    // Executes a heartbeat quorum round to confirm active leadership and acquire current read index
    Status GetReadIndex(Index* out_read_index, uint64_t timeout_ms = 500);

    // End-to-end linearizable point lookup on LSM state machine
    Status LinearizableGet(
        const Slice& key,
        std::string* value,
        std::shared_ptr<MemTableCore> memtable,
        RaftApplier* applier = nullptr,
        uint64_t timeout_ms = 500
    );

private:
    std::shared_ptr<RaftNode> raft_node_{nullptr};
    mutable std::mutex mutex_{};
};

} // namespace lsm
