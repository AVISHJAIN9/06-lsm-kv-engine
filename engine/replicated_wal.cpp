#include "replicated_wal.hpp"
#include <thread>
#include <chrono>

namespace lsm {

ReplicatedWAL::ReplicatedWAL(std::shared_ptr<RaftNode> raft_node)
    : raft_node_(std::move(raft_node)) {}

Status ReplicatedWAL::ProposeBatch(
    const std::string& serialized_batch,
    Index* out_index,
    Term* out_term
) {
    if (!raft_node_) {
        return Status::InvalidArgument("Null RaftNode in ReplicatedWAL");
    }

    Status s = raft_node_->Propose(serialized_batch, RaftEntryType::kData, out_index, out_term);
    if (!s.ok()) return s;

    // Trigger immediate peer replication
    raft_node_->BroadcastHeartbeats();
    return Status::OK();
}

Status ReplicatedWAL::WriteSync(const std::string& serialized_batch, uint64_t timeout_ms) {
    Index proposed_idx = 0;
    Term proposed_term = 0;

    Status s = ProposeBatch(serialized_batch, &proposed_idx, &proposed_term);
    if (!s.ok()) return s;

    auto start_time = std::chrono::steady_clock::now();
    while (true) {
        if (raft_node_->GetCommitIndex() >= proposed_idx) {
            return Status::OK();
        }

        if (raft_node_->GetCurrentTerm() > proposed_term || !raft_node_->IsLeader()) {
            return Status::Busy("Leadership lost before commit");
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time
        ).count();
        if (static_cast<uint64_t>(elapsed) >= timeout_ms) {
            return Status::Busy("ReplicatedWAL write timed out awaiting quorum commit");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        raft_node_->BroadcastHeartbeats();
    }
}

} // namespace lsm
