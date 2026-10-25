#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <atomic>
#include <optional>
#include <random>
#include "status.hpp"
#include "slice.hpp"

namespace lsm {

using NodeId = uint64_t;
using Term = uint64_t;
using Index = uint64_t;

enum class RaftRole {
    kFollower,
    kCandidate,
    kLeader
};

enum class RaftEntryType : uint8_t {
    kData = 1,
    kConfiguration = 2,
    kNoop = 3
};

struct RaftLogEntry {
    Term term{0};
    Index index{0};
    RaftEntryType type{RaftEntryType::kData};
    std::string payload{};
};

struct RequestVoteArgs {
    Term term{0};
    NodeId candidate_id{0};
    Index last_log_index{0};
    Term last_log_term{0};
};

struct RequestVoteReply {
    Term term{0};
    bool vote_granted{false};
};

struct AppendEntriesArgs {
    Term term{0};
    NodeId leader_id{0};
    Index prev_log_index{0};
    Term prev_log_term{0};
    std::vector<RaftLogEntry> entries{};
    Index leader_commit{0};
};

struct AppendEntriesReply {
    Term term{0};
    bool success{false};
    Index match_index{0};
    Index conflict_index{0};
    Term conflict_term{0};
};

struct InstallSnapshotArgs {
    Term term{0};
    NodeId leader_id{0};
    Index last_included_index{0};
    Term last_included_term{0};
    std::string data{};
};

struct InstallSnapshotReply {
    Term term{0};
};

class RaftNode;

// RaftNetworkTransport: Abstract network envelope dispatcher
class RaftNetworkTransport {
public:
    virtual ~RaftNetworkTransport() = default;

    virtual void SendRequestVote(
        NodeId target,
        const RequestVoteArgs& args,
        std::function<void(const RequestVoteReply&)> callback
    ) = 0;

    virtual void SendAppendEntries(
        NodeId target,
        const AppendEntriesArgs& args,
        std::function<void(const AppendEntriesReply&)> callback
    ) = 0;

    virtual void SendInstallSnapshot(
        NodeId target,
        const InstallSnapshotArgs& args,
        std::function<void(const InstallSnapshotReply&)> callback
    ) = 0;
};

// RaftNode: Core Raft Consensus State Machine adhering to Diego Ongaro specification (Feature 121).
class RaftNode {
public:
    RaftNode(
        NodeId self_id,
        std::vector<NodeId> peers,
        std::shared_ptr<RaftNetworkTransport> transport = nullptr
    );
    ~RaftNode() = default;

    RaftNode(const RaftNode&) = delete;
    RaftNode& operator=(const RaftNode&) = delete;

    NodeId GetId() const { return self_id_; }
    RaftRole GetRole() const;
    Term GetCurrentTerm() const;
    NodeId GetLeaderId() const;

    Index GetCommitIndex() const;
    Index GetLastApplied() const;
    Index GetLastLogIndex() const;
    Term GetLastLogTerm() const;

    void SetTransport(std::shared_ptr<RaftNetworkTransport> transport);

    // RPC Handlers
    RequestVoteReply HandleRequestVote(const RequestVoteArgs& args);
    AppendEntriesReply HandleAppendEntries(const AppendEntriesArgs& args);
    InstallSnapshotReply HandleInstallSnapshot(const InstallSnapshotArgs& args);

    // Timer & Trigger Events
    void TickElection(uint64_t elapsed_ms);
    void TickHeartbeat(uint64_t elapsed_ms);
    void TriggerElection();

    // Client Proposal on Leader
    Status Propose(
        const std::string& payload,
        RaftEntryType type,
        Index* out_index,
        Term* out_term
    );

    // Replication & Peer Progress Updates
    void BroadcastHeartbeats();
    void ReplicateToPeer(NodeId peer_id);
    void AdvanceCommitIndex();

    // Snapshotting & Log Compaction
    Status CompactLog(Index last_included_index, Term last_included_term, const std::string& snapshot_data);
    std::string GetSnapshotData() const;
    Index GetSnapshotLastIndex() const;
    Term GetSnapshotLastTerm() const;

    // Direct access to log entries
    bool GetEntry(Index index, RaftLogEntry* out_entry) const;
    std::vector<RaftLogEntry> GetEntriesFrom(Index start_index) const;

    // State inspection
    std::vector<NodeId> GetVoters() const;
    void SetVoters(const std::vector<NodeId>& voters);
    bool IsLeader() const { return GetRole() == RaftRole::kLeader; }

    void SetLastApplied(Index index);
    bool VerifyLeadershipQuorum();

private:
    void BecomeFollower(Term new_term);
    void BecomeCandidate();
    void BecomeLeader();

    Term GetLogTermAt(Index index) const;
    Index GetPhysicalLogIndex(Index logical_index) const;

    mutable std::mutex mutex_{};
    NodeId self_id_{0};
    std::vector<NodeId> peers_{};
    std::shared_ptr<RaftNetworkTransport> transport_{nullptr};

    // Persistent State
    Term current_term_{0};
    std::optional<NodeId> voted_for_{std::nullopt};
    std::vector<RaftLogEntry> log_{}; // 1-indexed logical entries

    // Snapshot State
    Index snapshot_last_index_{0};
    Term snapshot_last_term_{0};
    std::string snapshot_data_{};

    // Volatile State (All Servers)
    RaftRole role_{RaftRole::kFollower};
    NodeId current_leader_{0};
    Index commit_index_{0};
    Index last_applied_{0};

    // Volatile State (Leader Only)
    std::unordered_map<NodeId, Index> next_index_{};
    std::unordered_map<NodeId, Index> match_index_{};

    // Election Timer Configuration
    uint64_t election_timeout_ms_{200}; // Randomized 150 - 300 ms
    uint64_t election_elapsed_ms_{0};
    uint64_t heartbeat_interval_ms_{50};
    uint64_t heartbeat_elapsed_ms_{0};

    std::mt19937 rng_{std::random_device{}()};
    void ResetElectionTimer();
};

} // namespace lsm
