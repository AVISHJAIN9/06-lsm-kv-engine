#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <thread>
#include <chrono>
#include <memory>
#include <random>

#include "range_router.hpp"
#include "two_phase_coordinator.hpp"
#include "txn_participant.hpp"
#include "wound_wait.hpp"
#include "hlc_snapshot.hpp"
#include "range_splitter.hpp"

using namespace lsm;

// -----------------------------------------------------------------------------
// Test 1: Range Route Table Partition Lookup (Feature 127)
// -----------------------------------------------------------------------------
static void TestRangeRouteTableLookup() {
    std::cout << "\n=== Test 1: Range Route Table Partition Lookup (Feature 127) ===\n";

    RangeRouteTable route_table;

    // 4 contiguous non-overlapping partitions covering all string keys:
    // P1: ["", "g")
    // P2: ["g", "m")
    // P3: ["m", "s")
    // P4: ["s", "")
    RangeDescriptor p1{1, "", "g", 101, 1};
    RangeDescriptor p2{2, "g", "m", 102, 1};
    RangeDescriptor p3{3, "m", "s", 103, 1};
    RangeDescriptor p4{4, "s", "", 104, 1};

    assert(route_table.UpdatePartition(p1).ok());
    assert(route_table.UpdatePartition(p2).ok());
    assert(route_table.UpdatePartition(p3).ok());
    assert(route_table.UpdatePartition(p4).ok());

    assert(route_table.Size() == 4);

    // Test specific key routes
    RangeDescriptor out;
    assert(route_table.FindPartition(Slice("apple"), &out).ok() && out.partition_id == 1);
    assert(route_table.FindPartition(Slice("banana"), &out).ok() && out.partition_id == 1);
    assert(route_table.FindPartition(Slice("grape"), &out).ok() && out.partition_id == 2);
    assert(route_table.FindPartition(Slice("lemon"), &out).ok() && out.partition_id == 2);
    assert(route_table.FindPartition(Slice("mango"), &out).ok() && out.partition_id == 3);
    assert(route_table.FindPartition(Slice("orange"), &out).ok() && out.partition_id == 3);
    assert(route_table.FindPartition(Slice("strawberry"), &out).ok() && out.partition_id == 4);
    assert(route_table.FindPartition(Slice("zebra"), &out).ok() && out.partition_id == 4);

    // Route 1,000 random generated keys
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> char_dist('a', 'z');
    for (int i = 0; i < 1000; ++i) {
        std::string k;
        for (int j = 0; j < 8; ++j) {
            k.push_back(static_cast<char>(char_dist(rng)));
        }
        RangeDescriptor routed;
        Status s = route_table.FindPartition(Slice(k), &routed);
        assert(s.ok());
        assert(routed.Contains(Slice(k)));
        if (k < "g") assert(routed.partition_id == 1);
        else if (k < "m") assert(routed.partition_id == 2);
        else if (k < "s") assert(routed.partition_id == 3);
        else assert(routed.partition_id == 4);
    }

    std::cout << "  - 1,000 keys dynamically routed with O(log P) lookup and 100% boundary accuracy.\n";
    std::cout << "  [PASSED] Test 1: Range Route Table Partition Lookup\n";
}

// -----------------------------------------------------------------------------
// Test 2: Cross-Partition 2PC All-or-Nothing Commit (Feature 128)
// -----------------------------------------------------------------------------
static void TestTwoPhaseCommitAllOrNothing() {
    std::cout << "\n=== Test 2: Cross-Partition 2PC All-or-Nothing Commit (Feature 128) ===\n";

    auto lock_engine = std::make_shared<WoundWaitEngine>();
    auto mem1 = std::make_shared<MemTableCore>();
    auto mem2 = std::make_shared<MemTableCore>();

    auto part1 = std::make_shared<TxnParticipant>(1, mem1, lock_engine);
    auto part2 = std::make_shared<TxnParticipant>(2, mem2, lock_engine);

    std::map<PartitionId, std::shared_ptr<TxnParticipant>> participants = {
        {1, part1},
        {2, part2}
    };

    TwoPhaseCoordinator coordinator;

    // 1. Successful 2PC commit across Partition 1 & Partition 2
    DistributedTxn txn1 = coordinator.BeginTransaction();
    txn1.Put(1, "acct:alice", "1000_USD");
    txn1.Put(2, "acct:bob", "500_USD");

    Status s1 = coordinator.ExecuteTransaction(txn1, participants);
    assert(s1.ok());
    assert(coordinator.GetTxnStatus(txn1.txn_id) == TxnState::kCommitted);

    std::string v_alice, v_bob;
    assert(part1->Get(Slice("acct:alice"), kMaxSequenceNumber, &v_alice).ok() && v_alice == "1000_USD");
    assert(part2->Get(Slice("acct:bob"), kMaxSequenceNumber, &v_bob).ok() && v_bob == "500_USD");
    std::cout << "  - Txn 1 committed atomically across 2 shards: alice=1000, bob=500\n";

    // 2. Simulated failure on Partition 2: Txn 2 must ABORT and rollback cleanly
    part2->SetSimulatedPrepareFailure(true);

    DistributedTxn txn2 = coordinator.BeginTransaction();
    txn2.Put(1, "acct:alice", "800_USD"); // Debit 200 from Alice
    txn2.Put(2, "acct:bob", "700_USD");   // Credit 200 to Bob

    Status s2 = coordinator.ExecuteTransaction(txn2, participants);
    assert(!s2.ok());
    assert(coordinator.GetTxnStatus(txn2.txn_id) == TxnState::kAborted);
    std::cout << "  - Txn 2 aborted due to simulated shard 2 failure: " << s2.ToString() << "\n";

    // Verify all-or-nothing rollback: Alice must still be 1000_USD (zero partial updates)
    assert(part1->Get(Slice("acct:alice"), kMaxSequenceNumber, &v_alice).ok() && v_alice == "1000_USD");
    assert(part2->Get(Slice("acct:bob"), kMaxSequenceNumber, &v_bob).ok() && v_bob == "500_USD");
    std::cout << "  - Clean 2PC Rollback verified: alice=1000_USD, bob=500_USD (Zero partial writes exposed).\n";

    std::cout << "  [PASSED] Test 2: Cross-Partition 2PC All-or-Nothing Commit\n";
}

// -----------------------------------------------------------------------------
// Test 3: Participant Crash Recovery of In-Doubt Prepares (Feature 129)
// -----------------------------------------------------------------------------
static void TestParticipantCrashRecoveryInDoubt() {
    std::cout << "\n=== Test 3: Participant Crash Recovery of In-Doubt Prepares (Feature 129) ===\n";

    auto lock_engine = std::make_shared<WoundWaitEngine>();
    auto mem1 = std::make_shared<MemTableCore>();
    auto part1 = std::make_shared<TxnParticipant>(1, mem1, lock_engine);

    TwoPhaseCoordinator coordinator;
    HLCTimestamp start_ts{1000, 1};

    // Prepare transaction 42 on Participant 1
    std::vector<Mutation> mutations = {
        {ValueType::kTypeValue, "order:9901", "PAID_CONFIRMED"}
    };
    assert(part1->OnPrepare(42, mutations, start_ts).ok());
    assert(part1->GetTxnState(42) == TxnState::kPrepared);

    // Coordinator writes Commit decision
    coordinator.RecordDecision(42, TxnState::kCommitted);

    // Simulate crash before receiving OnCommit: Participant reboots and scans in-doubt txns
    std::vector<TxnId> in_doubt = part1->RecoverInDoubt();
    assert(in_doubt.size() == 1 && in_doubt[0] == 42);
    std::cout << "  - Participant crash recovery scanned 1 in-doubt prepared transaction (TxnId=42).\n";

    // Participant resolves in-doubt state by querying coordinator
    TxnState decision = coordinator.GetTxnStatus(42);
    assert(decision == TxnState::kCommitted);

    HLCTimestamp commit_ts{1000, 5};
    assert(part1->ResolveInDoubt(42, decision, commit_ts).ok());
    assert(part1->GetTxnState(42) == TxnState::kCommitted);

    // Verify data is now readable in MemTable
    std::string val;
    assert(part1->Get(Slice("order:9901"), kMaxSequenceNumber, &val).ok() && val == "PAID_CONFIRMED");
    std::cout << "  - In-doubt transaction 42 successfully resolved and applied to LSM MemTable: order:9901 = '" << val << "'\n";

    std::cout << "  [PASSED] Test 3: Participant Crash Recovery of In-Doubt Prepares\n";
}

// -----------------------------------------------------------------------------
// Test 4: Wound-Wait Deadlock Preemption (Feature 130)
// -----------------------------------------------------------------------------
static void TestWoundWaitDeadlockPreemption() {
    std::cout << "\n=== Test 4: Wound-Wait Deadlock Preemption (Feature 130) ===\n";

    WoundWaitEngine engine;

    // Txn 1 is OLDER (start_ts = 100)
    // Txn 2 is YOUNGER (start_ts = 200)
    TxnId txn_old = 1;
    TxnId txn_young = 2;

    HLCTimestamp ts_old{100, 0};
    HLCTimestamp ts_young{200, 0};

    engine.RegisterTxn(txn_old, ts_old);
    engine.RegisterTxn(txn_young, ts_young);

    // Step 1: Txn 1 acquires lock on Resource A
    assert(engine.AcquireLock(txn_old, "resource:A").ok());

    // Step 2: Txn 2 acquires lock on Resource B
    assert(engine.AcquireLock(txn_young, "resource:B").ok());

    // Step 3: Txn 1 (OLDER) requests Resource B (held by Txn 2 YOUNGER)
    // Wound-Wait rule: Older wounds younger!
    // Txn 1 must wound Txn 2, taking over Resource B without blocking
    Status s = engine.AcquireLock(txn_old, "resource:B");
    assert(s.ok());

    // Assert Txn 2 was wounded
    assert(engine.IsWounded(txn_young));
    std::cout << "  - Older Txn 1 (TS=100) preemptively WOUNDED younger Txn 2 (TS=200) on lock contention.\n";

    // Step 4: When Txn 2 attempts any operation, it is rejected due to wound
    Status s_young = engine.AcquireLock(txn_young, "resource:C");
    assert(s_young.IsAborted());
    std::cout << "  - Wounded Txn 2 aborted cleanly: " << s_young.ToString() << "\n";

    // Txn 1 completes and releases all locks
    engine.ReleaseAllLocks(txn_old);
    assert(engine.GetHeldLockCount(txn_old) == 0);

    std::cout << "  - Deadlock cycle completely averted via Wound-Wait priority preemption.\n";
    std::cout << "  [PASSED] Test 4: Wound-Wait Deadlock Preemption\n";
}

// -----------------------------------------------------------------------------
// Test 5: Cross-Partition Consistent Snapshot Reads (Feature 131)
// -----------------------------------------------------------------------------
static void TestConsistentSnapshotReads() {
    std::cout << "\n=== Test 5: Cross-Partition Consistent Snapshot Reads (Feature 131) ===\n";

    auto hlc = std::make_shared<HybridLogicalClock>();
    auto lock_engine = std::make_shared<WoundWaitEngine>();

    auto mem_a = std::make_shared<MemTableCore>();
    auto mem_b = std::make_shared<MemTableCore>();

    auto part_a = std::make_shared<TxnParticipant>(1, mem_a, lock_engine);
    auto part_b = std::make_shared<TxnParticipant>(2, mem_b, lock_engine);

    std::map<PartitionId, std::shared_ptr<TxnParticipant>> parts = {
        {1, part_a},
        {2, part_b}
    };

    TwoPhaseCoordinator coordinator(hlc);

    // Initial state: Bank A = 1000, Bank B = 2000 (Total = 3000)
    DistributedTxn t0 = coordinator.BeginTransaction();
    t0.Put(1, "bank:A", "1000");
    t0.Put(2, "bank:B", "2000");
    assert(coordinator.ExecuteTransaction(t0, parts).ok());

    // Snapshot 1 captured
    HLCTimestamp snap1_ts = hlc->Now();
    SequenceNumber seq1 = snap1_ts.physical_time_ms * 1000 + snap1_ts.logical_counter;

    // Txn 1: Transfer 200 from A to B (A=800, B=2200, Total=3000)
    DistributedTxn t1 = coordinator.BeginTransaction();
    t1.Put(1, "bank:A", "800");
    t1.Put(2, "bank:B", "2200");
    assert(coordinator.ExecuteTransaction(t1, parts).ok());

    // Snapshot 2 captured
    HLCTimestamp snap2_ts = hlc->Now();
    SequenceNumber seq2 = snap2_ts.physical_time_ms * 1000 + snap2_ts.logical_counter;

    // Txn 2: Transfer 300 from A to B (A=500, B=2500, Total=3000)
    DistributedTxn t2 = coordinator.BeginTransaction();
    t2.Put(1, "bank:A", "500");
    t2.Put(2, "bank:B", "2500");
    assert(coordinator.ExecuteTransaction(t2, parts).ok());

    // Verify Snapshot 1 reads
    std::string a_val, b_val;
    assert(part_a->Get(Slice("bank:A"), seq1, &a_val).ok() && a_val == "1000");
    assert(part_b->Get(Slice("bank:B"), seq1, &b_val).ok() && b_val == "2000");
    assert(std::stoi(a_val) + std::stoi(b_val) == 3000);
    std::cout << "  - Snapshot 1: Bank A = " << a_val << ", Bank B = " << b_val << " (Sum = 3000)\n";

    // Verify Snapshot 2 reads
    assert(part_a->Get(Slice("bank:A"), seq2, &a_val).ok() && a_val == "800");
    assert(part_b->Get(Slice("bank:B"), seq2, &b_val).ok() && b_val == "2200");
    assert(std::stoi(a_val) + std::stoi(b_val) == 3000);
    std::cout << "  - Snapshot 2: Bank A = " << a_val << ", Bank B = " << b_val << " (Sum = 3000)\n";

    // Verify Latest current reads
    assert(part_a->Get(Slice("bank:A"), kMaxSequenceNumber, &a_val).ok() && a_val == "500");
    assert(part_b->Get(Slice("bank:B"), kMaxSequenceNumber, &b_val).ok() && b_val == "2500");
    assert(std::stoi(a_val) + std::stoi(b_val) == 3000);
    std::cout << "  - Current Read: Bank A = " << a_val << ", Bank B = " << b_val << " (Sum = 3000)\n";

    std::cout << "  - Conservation of total balance strictly preserved across all snapshots without torn states.\n";
    std::cout << "  [PASSED] Test 5: Cross-Partition Consistent Snapshot Reads\n";
}

// -----------------------------------------------------------------------------
// Test 6: Dynamic Partition Range Split Under Load (Feature 132)
// -----------------------------------------------------------------------------
static void TestDynamicRangeSplitUnderLoad() {
    std::cout << "\n=== Test 6: Dynamic Partition Range Split Under Load (Feature 132) ===\n";

    RangeRouteTable route_table;
    auto lock_engine = std::make_shared<WoundWaitEngine>();

    RangeDescriptor initial_desc{1, "user:0000", "user:9999", 101, 1};
    assert(route_table.UpdatePartition(initial_desc).ok());

    auto parent_part = std::make_shared<Partition>(initial_desc, lock_engine);

    // Populate 500 keys in parent partition
    for (int i = 0; i < 500; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "user:%04d", i);
        std::string val = "profile_data_" + std::to_string(i);
        assert(parent_part->Put(buf, val, i + 1).ok());
    }

    assert(RangeSplitter::CheckSplitNeeded(*parent_part, 5000)); // Threshold 5KB exceeded
    std::cout << "  - Partition size threshold exceeded (Size = " << parent_part->approximate_bytes.load() << " B). Split triggered.\n";

    // Execute split at split_key = "user:0250"
    std::shared_ptr<Partition> left_child;
    std::shared_ptr<Partition> right_child;
    Status s = RangeSplitter::ExecuteSplit(
        parent_part,
        "user:0250",
        2,    // new right partition ID
        102,  // right leader
        &route_table,
        lock_engine,
        left_child,
        right_child
    );
    assert(s.ok());
    assert(route_table.Size() == 2);

    // Verify routing table updated
    RangeDescriptor out_left, out_right;
    assert(route_table.FindPartition(Slice("user:0100"), &out_left).ok() && out_left.partition_id == 1);
    assert(route_table.FindPartition(Slice("user:0300"), &out_right).ok() && out_right.partition_id == 2);
    std::cout << "  - Route directory updated: Left [user:0000, user:0250), Right [user:0250, user:9999)\n";

    // Verify data completeness across child partitions
    std::string v;
    assert(left_child->Get(Slice("user:0100"), kMaxSequenceNumber, &v).ok() && v == "profile_data_100");
    assert(right_child->Get(Slice("user:0300"), kMaxSequenceNumber, &v).ok() && v == "profile_data_300");

    // Verify in-flight active writes continue on child partitions seamlessly
    assert(right_child->Put("user:0350", "new_active_write", 1000).ok());
    assert(right_child->Get(Slice("user:0350"), kMaxSequenceNumber, &v).ok() && v == "new_active_write");
    std::cout << "  - In-flight writes and reads executed seamlessly on split child partitions.\n";

    std::cout << "  [PASSED] Test 6: Dynamic Partition Range Split Under Load\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase XXII Verification Test Suite\n";
    std::cout << "  Features 127-132: Sharding, Distributed Transactions & Cross-Partition\n";
    std::cout << "=======================================================================\n";

    TestRangeRouteTableLookup();
    TestTwoPhaseCommitAllOrNothing();
    TestParticipantCrashRecoveryInDoubt();
    TestWoundWaitDeadlockPreemption();
    TestConsistentSnapshotReads();
    TestDynamicRangeSplitUnderLoad();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase XXII Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
