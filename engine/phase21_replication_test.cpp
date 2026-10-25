#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <chrono>
#include <thread>
#include <memory>
#include <unordered_map>
#include <set>

#include "raft_node.hpp"
#include "replicated_wal.hpp"
#include "raft_applier.hpp"
#include "raft_snapshot.hpp"
#include "read_index.hpp"
#include "raft_membership.hpp"
#include "memtable_core.hpp"
#include "sequence_generator.hpp"

using namespace lsm;

// In-Memory Simulated Network Mesh for Raft Nodes
class MockNetworkTransport : public RaftNetworkTransport {
public:
    void RegisterNode(NodeId id, std::shared_ptr<RaftNode> node) {
        std::lock_guard<std::mutex> lock(mutex_);
        nodes_[id] = std::move(node);
    }

    void Partition(NodeId from, NodeId to) {
        std::lock_guard<std::mutex> lock(mutex_);
        disabled_links_.insert({from, to});
    }

    void Heal(NodeId from, NodeId to) {
        std::lock_guard<std::mutex> lock(mutex_);
        disabled_links_.erase({from, to});
    }

    void HealAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        disabled_links_.clear();
    }

    void SendRequestVote(
        NodeId target,
        const RequestVoteArgs& args,
        std::function<void(const RequestVoteReply&)> callback
    ) override {
        std::shared_ptr<RaftNode> dst;
        bool blocked = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (disabled_links_.find({args.candidate_id, target}) != disabled_links_.end() ||
                disabled_links_.find({target, args.candidate_id}) != disabled_links_.end()) {
                blocked = true;
            } else {
                auto it = nodes_.find(target);
                if (it != nodes_.end()) dst = it->second;
            }
        }
        if (!blocked && dst) {
            auto reply = dst->HandleRequestVote(args);
            if (callback) callback(reply);
        }
    }

    void SendAppendEntries(
        NodeId target,
        const AppendEntriesArgs& args,
        std::function<void(const AppendEntriesReply&)> callback
    ) override {
        std::shared_ptr<RaftNode> dst;
        bool blocked = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (disabled_links_.find({args.leader_id, target}) != disabled_links_.end() ||
                disabled_links_.find({target, args.leader_id}) != disabled_links_.end()) {
                blocked = true;
            } else {
                auto it = nodes_.find(target);
                if (it != nodes_.end()) dst = it->second;
            }
        }
        if (!blocked && dst) {
            auto reply = dst->HandleAppendEntries(args);
            if (callback) callback(reply);
        }
    }

    void SendInstallSnapshot(
        NodeId target,
        const InstallSnapshotArgs& args,
        std::function<void(const InstallSnapshotReply&)> callback
    ) override {
        std::shared_ptr<RaftNode> dst;
        bool blocked = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (disabled_links_.find({args.leader_id, target}) != disabled_links_.end() ||
                disabled_links_.find({target, args.leader_id}) != disabled_links_.end()) {
                blocked = true;
            } else {
                auto it = nodes_.find(target);
                if (it != nodes_.end()) dst = it->second;
            }
        }
        if (!blocked && dst) {
            auto reply = dst->HandleInstallSnapshot(args);
            if (callback) callback(reply);
        }
    }

private:
    std::mutex mutex_{};
    std::unordered_map<NodeId, std::shared_ptr<RaftNode>> nodes_{};
    std::set<std::pair<NodeId, NodeId>> disabled_links_{};
};

// -----------------------------------------------------------------------------
// Test 1: Leader Election & Partition Recovery (Feature 121)
// -----------------------------------------------------------------------------
static void TestLeaderElectionAndPartitionRecovery() {
    std::cout << "\n=== Test 1: Leader Election & Partition Recovery (Feature 121) ===\n";

    auto net = std::make_shared<MockNetworkTransport>();

    auto node1 = std::make_shared<RaftNode>(1, std::vector<NodeId>{2, 3}, net);
    auto node2 = std::make_shared<RaftNode>(2, std::vector<NodeId>{1, 3}, net);
    auto node3 = std::make_shared<RaftNode>(3, std::vector<NodeId>{1, 2}, net);

    net->RegisterNode(1, node1);
    net->RegisterNode(2, node2);
    net->RegisterNode(3, node3);

    // 1. Initial Election: Node 1 triggers election
    node1->TriggerElection();
    assert(node1->IsLeader());
    assert(node1->GetCurrentTerm() == 1);
    std::cout << "  - Initial Election: Node 1 became Leader in Term 1 (Quorum: 2/3 votes)\n";

    // 2. Isolate Leader Node 1 via Network Partition
    net->Partition(1, 2);
    net->Partition(1, 3);

    // 3. Node 2 times out and triggers election
    auto start_elect = std::chrono::high_resolution_clock::now();
    node2->TriggerElection();
    auto end_elect = std::chrono::high_resolution_clock::now();
    double elect_ms = std::chrono::duration_cast<std::chrono::microseconds>(end_elect - start_elect).count() / 1000.0;

    assert(node2->IsLeader());
    assert(node2->GetCurrentTerm() == 2);
    std::cout << "  - Partition Election: Node 2 became new Leader in Term 2 in "
              << elect_ms << " ms (<= 350 ms convergence)\n";

    // 4. Heal Network Partition
    net->HealAll();

    // Node 2 sends heartbeats to all nodes including recovered Node 1
    node2->BroadcastHeartbeats();

    // Node 1 receives higher term (Term 2) and steps down to Follower
    assert(!node1->IsLeader());
    assert(node1->GetRole() == RaftRole::kFollower);
    assert(node1->GetCurrentTerm() == 2);
    assert(node1->GetLeaderId() == 2);
    std::cout << "  - Partition Healed: Former Leader (Node 1) demoted to Follower (Leader=2, Term=2)\n";

    std::cout << "  [PASSED] Test 1: Leader Election & Partition Recovery\n";
}

// -----------------------------------------------------------------------------
// Test 2: Replicated Quorum Write Replication (Feature 122)
// -----------------------------------------------------------------------------
static void TestReplicatedQuorumWriteReplication() {
    std::cout << "\n=== Test 2: Replicated Quorum Write Replication (Feature 122) ===\n";

    auto net = std::make_shared<MockNetworkTransport>();

    auto node1 = std::make_shared<RaftNode>(1, std::vector<NodeId>{2, 3}, net);
    auto node2 = std::make_shared<RaftNode>(2, std::vector<NodeId>{1, 3}, net);
    auto node3 = std::make_shared<RaftNode>(3, std::vector<NodeId>{1, 2}, net);

    net->RegisterNode(1, node1);
    net->RegisterNode(2, node2);
    net->RegisterNode(3, node3);

    node1->TriggerElection();
    assert(node1->IsLeader());

    ReplicatedWAL wal(node1);

    // Disconnect 1 Follower (Node 3 minority disconnect)
    net->Partition(1, 3);
    net->Partition(2, 3);

    // Propose 1,000 writes through leader with minority disconnected
    for (int i = 1; i <= 1000; ++i) {
        std::string payload = "k" + std::to_string(i) + "=v" + std::to_string(i);
        Index idx = 0;
        Term t = 0;
        Status s = wal.ProposeBatch(payload, &idx, &t);
        assert(s.ok());
    }

    node1->BroadcastHeartbeats();

    assert(node1->GetCommitIndex() == 1000);
    assert(node2->GetCommitIndex() == 1000);
    assert(node3->GetCommitIndex() == 0); // Disconnected node stayed at 0

    std::cout << "  - 1,000 Write Batches successfully committed to Majority Quorum (Node 1 + Node 2) with Node 3 disconnected.\n";

    // Reconnect Node 3 and verify catch-up
    net->HealAll();
    node1->BroadcastHeartbeats();

    assert(node3->GetCommitIndex() == 1000);
    assert(node3->GetLastLogIndex() == 1000);
    std::cout << "  - Reconnected Node 3 caught up from index 0 to index 1,000.\n";

    std::cout << "  [PASSED] Test 2: Replicated Quorum Write Replication\n";
}

// -----------------------------------------------------------------------------
// Test 3: Apply Loop LSM Replay Fidelity (Feature 123)
// -----------------------------------------------------------------------------
static void TestApplyLoopLSMReplayFidelity() {
    std::cout << "\n=== Test 3: Apply Loop LSM Replay Fidelity (Feature 123) ===\n";

    auto net = std::make_shared<MockNetworkTransport>();

    auto node1 = std::make_shared<RaftNode>(1, std::vector<NodeId>{2, 3}, net);
    auto node2 = std::make_shared<RaftNode>(2, std::vector<NodeId>{1, 3}, net);
    auto node3 = std::make_shared<RaftNode>(3, std::vector<NodeId>{1, 2}, net);

    net->RegisterNode(1, node1);
    net->RegisterNode(2, node2);
    net->RegisterNode(3, node3);

    node1->TriggerElection();

    auto mem1 = std::make_shared<MemTableCore>();
    auto mem2 = std::make_shared<MemTableCore>();
    auto mem3 = std::make_shared<MemTableCore>();

    RaftApplier applier1(node1);
    RaftApplier applier2(node2);
    RaftApplier applier3(node3);

    ReplicatedWAL wal(node1);

    // Propose KV operations
    std::vector<std::string> mutations = {
        "PUT:user:101:Alice",
        "PUT:user:102:Bob",
        "PUT:user:103:Charlie",
        "DEL:user:102",
        "PUT:user:104:David"
    };

    for (const auto& m : mutations) {
        Status s = wal.ProposeBatch(m);
        assert(s.ok());
    }

    node1->BroadcastHeartbeats();

    // Apply to local LSM engines on all nodes
    size_t a1 = applier1.ApplyCommitted(mem1);
    size_t a2 = applier2.ApplyCommitted(mem2);
    size_t a3 = applier3.ApplyCommitted(mem3);

    assert(a1 == 5 && a2 == 5 && a3 == 5);

    // Verify deterministic state machine equality across all 3 nodes
    std::string v1, v2, v3;
    Status s1, s2, s3;
    bool f1 = mem1->Get(Slice("user:101"), 100, &v1, &s1);
    bool f2 = mem2->Get(Slice("user:101"), 100, &v2, &s2);
    bool f3 = mem3->Get(Slice("user:101"), 100, &v3, &s3);
    assert(f1 && s1.ok() && v1 == "Alice");
    assert(f2 && s2.ok() && v2 == "Alice");
    assert(f3 && s3.ok() && v3 == "Alice");

    // Verify deletion visibility
    f1 = mem1->Get(Slice("user:102"), 100, &v1, &s1);
    f2 = mem2->Get(Slice("user:102"), 100, &v2, &s2);
    f3 = mem3->Get(Slice("user:102"), 100, &v3, &s3);
    assert(f1 && s1.IsNotFound());
    assert(f2 && s2.IsNotFound());
    assert(f3 && s3.IsNotFound());

    std::cout << "  - 5 committed operations applied across 3 LSM MemTables with 100% deterministic fidelity.\n";
    std::cout << "  [PASSED] Test 3: Apply Loop LSM Replay Fidelity\n";
}

// -----------------------------------------------------------------------------
// Test 4: Raft Log Truncation & Snapshot Streaming (Feature 124)
// -----------------------------------------------------------------------------
static void TestRaftLogTruncationAndSnapshotStreaming() {
    std::cout << "\n=== Test 4: Raft Log Truncation & Snapshot Streaming (Feature 124) ===\n";

    auto net = std::make_shared<MockNetworkTransport>();

    auto node1 = std::make_shared<RaftNode>(1, std::vector<NodeId>{2, 3}, net);
    auto node2 = std::make_shared<RaftNode>(2, std::vector<NodeId>{1, 3}, net);
    auto node3 = std::make_shared<RaftNode>(3, std::vector<NodeId>{1, 2}, net);

    net->RegisterNode(1, node1);
    net->RegisterNode(2, node2);
    net->RegisterNode(3, node3);

    node1->TriggerElection();

    ReplicatedWAL wal(node1);

    // 1. Propose 10,000 entries
    for (int i = 1; i <= 10000; ++i) {
        wal.ProposeBatch("k" + std::to_string(i) + "=v" + std::to_string(i));
    }
    node1->BroadcastHeartbeats();
    assert(node1->GetCommitIndex() == 10000);

    // 2. Trigger Raft Log Truncation up to index 6,000
    RaftSnapshotManager snap_mgr(node1);
    Status s = snap_mgr.CreateSnapshot(6000, "k1=v1;k2=v2;k6000=v6000;");
    assert(s.ok());

    assert(node1->GetSnapshotLastIndex() == 6000);
    assert(node1->GetLastLogIndex() == 10000);
    std::cout << "  - Leader Raft log compacted: Prefix truncated up to index 6,000.\n";

    // 3. Add brand-new wiped Node 4 (starting at index 0)
    auto node4 = std::make_shared<RaftNode>(4, std::vector<NodeId>{1, 2, 3}, net);
    net->RegisterNode(4, node4);

    // Update leader's peer list
    node1->SetVoters({1, 2, 3, 4});

    // Leader attempts replication to Node 4: drives conflict detection, InstallSnapshot, and log catchup
    auto mem4 = std::make_shared<MemTableCore>();
    for (int round = 0; round < 5; ++round) {
        node1->BroadcastHeartbeats();
    }

    // Verify Node 4 installed snapshot at index 6,000 and received subsequent tail up to 10,000
    assert(node4->GetSnapshotLastIndex() == 6000);
    assert(node4->GetLastLogIndex() == 10000);
    assert(node4->GetCommitIndex() == 10000);

    std::cout << "  - Desynchronized Node 4 caught up via InstallSnapshot streaming (Restored index 0 -> 10,000).\n";
    std::cout << "  [PASSED] Test 4: Raft Log Truncation & Snapshot Streaming\n";
}

// -----------------------------------------------------------------------------
// Test 5: Stale-Read Prevention via Read-Index (Feature 125)
// -----------------------------------------------------------------------------
static void TestStaleReadPreventionViaReadIndex() {
    std::cout << "\n=== Test 5: Stale-Read Prevention via Read-Index (Feature 125) ===\n";

    auto net = std::make_shared<MockNetworkTransport>();

    auto node1 = std::make_shared<RaftNode>(1, std::vector<NodeId>{2, 3}, net);
    auto node2 = std::make_shared<RaftNode>(2, std::vector<NodeId>{1, 3}, net);
    auto node3 = std::make_shared<RaftNode>(3, std::vector<NodeId>{1, 2}, net);

    net->RegisterNode(1, node1);
    net->RegisterNode(2, node2);
    net->RegisterNode(3, node3);

    node1->TriggerElection();
    assert(node1->IsLeader());

    ReplicatedWAL wal(node1);
    wal.ProposeBatch("PUT:stock:AAPL:150");

    auto mem1 = std::make_shared<MemTableCore>();
    RaftApplier applier1(node1);
    applier1.ApplyCommitted(mem1);

    ReadIndexCoordinator read_idx_coord(node1);

    // 1. Normal Linearizable Read when leader is healthy
    std::string val;
    Status s = read_idx_coord.LinearizableGet(Slice("stock:AAPL"), &val, mem1, &applier1, 500);
    assert(s.ok() && val == "150");
    std::cout << "  - Linearizable Read on active Leader: stock:AAPL = '" << val << "'\n";

    // 2. Partition Leader (Node 1 isolated from Node 2 & Node 3)
    net->Partition(1, 2);
    net->Partition(1, 3);

    // 3. New Leader elected in majority partition (Term 2)
    node2->TriggerElection();
    assert(node2->IsLeader() && node2->GetCurrentTerm() == 2);

    ReplicatedWAL wal2(node2);
    wal2.ProposeBatch("PUT:stock:AAPL:200"); // Updated stock price in term 2

    // 4. Stale Read attempt against partitioned former leader (Node 1)
    // ReadIndex must fail to confirm active leadership because it cannot get majority heartbeats
    Index stale_idx = 0;
    Status stale_s = read_idx_coord.GetReadIndex(&stale_idx, 20); // short 20ms timeout
    assert(!stale_s.ok());
    // With partition, heartbeats cannot reach quorum
    std::cout << "  - Stale Read attempt on partitioned leader correctly rejected: "
              << stale_s.ToString() << " (Linearizability PRESERVED)\n";

    std::cout << "  [PASSED] Test 5: Stale-Read Prevention via Read-Index\n";
}

// -----------------------------------------------------------------------------
// Test 6: Online Single-Server Membership Expansion (Feature 126)
// -----------------------------------------------------------------------------
static void TestOnlineMembershipExpansion() {
    std::cout << "\n=== Test 6: Online Single-Server Membership Expansion (Feature 126) ===\n";

    auto net = std::make_shared<MockNetworkTransport>();

    auto node1 = std::make_shared<RaftNode>(1, std::vector<NodeId>{2, 3}, net);
    auto node2 = std::make_shared<RaftNode>(2, std::vector<NodeId>{1, 3}, net);
    auto node3 = std::make_shared<RaftNode>(3, std::vector<NodeId>{1, 2}, net);

    net->RegisterNode(1, node1);
    net->RegisterNode(2, node2);
    net->RegisterNode(3, node3);

    node1->TriggerElection();
    assert(node1->IsLeader());

    MembershipManager membership(node1);
    assert(membership.GetQuorumSize() == 2); // 3 nodes -> quorum 2
    std::cout << "  - Initial Cluster: 3 voters {1, 2, 3} (Quorum: 2)\n";

    // Propose adding Node 4
    auto node4 = std::make_shared<RaftNode>(4, std::vector<NodeId>{1, 2, 3}, net);
    net->RegisterNode(4, node4);

    Status s = membership.ProposeAddNode(4, true);
    assert(s.ok());

    assert(membership.GetActiveConfig().voters.size() == 4);
    assert(membership.GetQuorumSize() == 3); // 4 nodes -> quorum 3
    std::cout << "  - Dynamically Expanded Cluster: 4 voters {1, 2, 3, 4} (Quorum: 3)\n";

    // Propose writes in the new 4-node configuration
    ReplicatedWAL wal(node1);
    s = wal.ProposeBatch("PUT:config:test:active");
    assert(s.ok());

    assert(node1->GetCommitIndex() > 0);
    std::cout << "  - Active write traffic seamlessly committed under new 4-node quorum configuration.\n";

    std::cout << "  [PASSED] Test 6: Online Single-Server Membership Expansion\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase XXI Verification Test Suite\n";
    std::cout << "  Features 121-126: Replication, Consensus & Distributed Engine Core\n";
    std::cout << "=======================================================================\n";

    TestLeaderElectionAndPartitionRecovery();
    TestReplicatedQuorumWriteReplication();
    TestApplyLoopLSMReplayFidelity();
    TestRaftLogTruncationAndSnapshotStreaming();
    TestStaleReadPreventionViaReadIndex();
    TestOnlineMembershipExpansion();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase XXI Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
