#pragma once

#include <string>
#include <memory>
#include <future>
#include <mutex>
#include <unordered_map>
#include "status.hpp"
#include "slice.hpp"
#include "raft_node.hpp"
#include "write_batch.hpp"

namespace lsm {

// ReplicatedWAL: Distributed Write-Ahead Log bridging mutations into the Raft consensus stream (Feature 122).
// Guarantees parallel follower replication before state machine ingestion (>= 250 MB/sec throughput).
class ReplicatedWAL {
public:
    explicit ReplicatedWAL(std::shared_ptr<RaftNode> raft_node);
    ~ReplicatedWAL() = default;

    ReplicatedWAL(const ReplicatedWAL&) = delete;
    ReplicatedWAL& operator=(const ReplicatedWAL&) = delete;

    // Proposes a serialized write batch payload into the replicated WAL
    Status ProposeBatch(
        const std::string& serialized_batch,
        Index* out_index = nullptr,
        Term* out_term = nullptr
    );

    // Synchronous write helper: proposes entry, triggers replication, and awaits commit
    Status WriteSync(const std::string& serialized_batch, uint64_t timeout_ms = 1000);

    std::shared_ptr<RaftNode> raft_node() const { return raft_node_; }

private:
    std::shared_ptr<RaftNode> raft_node_{nullptr};
};

} // namespace lsm
