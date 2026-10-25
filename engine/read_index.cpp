#include "read_index.hpp"
#include <thread>
#include <chrono>

namespace lsm {

ReadIndexCoordinator::ReadIndexCoordinator(std::shared_ptr<RaftNode> raft_node)
    : raft_node_(std::move(raft_node)) {}

Status ReadIndexCoordinator::GetReadIndex(Index* out_read_index, uint64_t /* timeout_ms */) {
    if (!raft_node_) {
        return Status::InvalidArgument("Null RaftNode in ReadIndexCoordinator");
    }

    if (!raft_node_->IsLeader()) {
        return Status::InvalidArgument("Not cluster leader (current leader: " +
                                      std::to_string(raft_node_->GetLeaderId()) + ")");
    }

    Index read_idx = raft_node_->GetCommitIndex();

    // Verify active leadership quorum via heartbeat barrier round
    bool quorum_ok = raft_node_->VerifyLeadershipQuorum();
    if (!quorum_ok) {
        return Status::Busy("Read-Index heartbeat quorum failed: partitioned leader");
    }

    if (out_read_index) *out_read_index = read_idx;
    return Status::OK();
}

Status ReadIndexCoordinator::LinearizableGet(
    const Slice& key,
    std::string* value,
    std::shared_ptr<MemTableCore> memtable,
    RaftApplier* applier,
    uint64_t timeout_ms
) {
    if (!memtable) {
        return Status::InvalidArgument("Null MemTable in LinearizableGet");
    }

    Index read_index = 0;
    Status s = GetReadIndex(&read_index, timeout_ms);
    if (!s.ok()) return s;

    // Apply any committed entries up to read_index
    if (applier) {
        applier->ApplyCommitted(memtable);
        if (applier->GetLastApplied() < read_index) {
            return Status::Busy("State machine catch-up pending");
        }
    }

    // Point lookup in MemTable
    Status s_get;
    bool found = memtable->Get(key, kMaxSequenceNumber, value, &s_get);
    if (!found) {
        return Status::NotFound("Key not found");
    }
    return s_get;
}

} // namespace lsm

