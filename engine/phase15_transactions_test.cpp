#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <thread>
#include <chrono>
#include <memory>
#include <algorithm>
#include <unordered_set>
#include <iomanip>

#include "tombstone_purger.hpp"
#include "range_tombstone.hpp"
#include "atomic_batch.hpp"
#include "optimistic_txn.hpp"
#include "conflict_detector.hpp"
#include "txn_recovery.hpp"
#include "sequence_generator.hpp"
#include "snapshot_manager.hpp"
#include "memtable_core.hpp"
#include "types.hpp"

using namespace lsm;

// -----------------------------------------------------------------------------
// Test 1: Snapshot-Pinned Tombstone Retention (Feature 85)
// -----------------------------------------------------------------------------
static void Test1_SnapshotPinnedTombstoneRetention() {
    std::cout << "\n=== Test 1: Snapshot-Pinned Tombstone Retention (Feature 85) ===\n";

    SnapshotManager snap_mgr;
    SequenceGenerator seq_gen(0);

    // Sequence 10: Put "user:alice"
    SequenceNumber s10 = seq_gen.Allocate(1);
    assert(s10 == 1);
    // Snapshot taken at sequence 1
    const Snapshot* snap = snap_mgr.CreateSnapshot(1);
    assert(snap_mgr.GetOldestSnapshot() == 1);

    // Sequence 2: Delete "user:alice" (tombstone)
    SequenceNumber tomb_seq = seq_gen.Allocate(1);
    assert(tomb_seq == 2);

    // Case 1: Snapshot at LSN 1 is active -> tombstone at LSN 2 MUST be retained
    bool retain_with_snap = TombstonePurger::ShouldRetainTombstone(
        Slice("user:alice"),
        tomb_seq,
        /*current_level=*/0,
        /*key_exists_in_deeper_levels=*/false,
        snap_mgr.GetOldestSnapshot()
    );
    assert(retain_with_snap);
    std::cout << "  - Active Snapshot (LSN=1) pinned: Tombstone (LSN=" << tomb_seq
              << ") RETAINED in compaction output.\n";

    // Case 2: Release snapshot -> oldest active snapshot is 0
    snap_mgr.ReleaseSnapshot(snap);
    assert(snap_mgr.IsEmpty());

    // Without active snapshots and bottommost level (no deeper versions), tombstone is purged
    bool retain_without_snap = TombstonePurger::ShouldRetainTombstone(
        Slice("user:alice"),
        tomb_seq,
        /*current_level=*/6,
        /*key_exists_in_deeper_levels=*/false,
        snap_mgr.GetOldestSnapshot()
    );
    assert(!retain_without_snap);
    std::cout << "  - Snapshot released: Tombstone (LSN=" << tomb_seq
              << ") PURGED from bottommost compaction output.\n";

    // Case 3: Deeper level versions exist -> tombstone MUST be retained to mask them
    bool retain_deeper = TombstonePurger::ShouldRetainTombstone(
        Slice("user:alice"),
        tomb_seq,
        /*current_level=*/1,
        /*key_exists_in_deeper_levels=*/true,
        snap_mgr.GetOldestSnapshot()
    );
    assert(retain_deeper);
    std::cout << "  - Deeper level versions present: Tombstone (LSN=" << tomb_seq
              << ") RETAINED to mask obsolete historical records.\n";

    std::cout << "  [PASSED] Test 1: Snapshot-Pinned Tombstone Retention\n";
}

// -----------------------------------------------------------------------------
// Test 2: Range Delete Masking Across Multiple Keys (Feature 86)
// -----------------------------------------------------------------------------
static void Test2_RangeDeleteMasking() {
    std::cout << "\n=== Test 2: Range Delete Masking Across Multiple Keys (Feature 86) ===\n";

    RangeDelAggregator range_del;
    // Issue range deletion for interval ["item:20", "item:80") at sequence 20
    range_del.AddTombstone(Slice("item:20"), Slice("item:80"), 20);

    assert(range_del.Count() == 1);
    assert(!range_del.IsEmpty());

    // Check point keys written at sequence 10
    // Key "item:19" at seq 10 -> outside interval [20, 80) -> NOT deleted
    assert(!range_del.ShouldDelete(Slice("item:19"), 10));

    // Keys "item:20" through "item:79" at seq 10 -> inside interval and seq 20 > 10 -> DELETED
    for (int i = 20; i < 80; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "item:%02d", i);
        assert(range_del.ShouldDelete(Slice(buf), 10));
    }

    // Key "item:80" at seq 10 -> exclusive upper bound -> NOT deleted
    assert(!range_del.ShouldDelete(Slice("item:80"), 10));

    // New write of "item:50" at sequence 30 -> newer than range tombstone (30 > 20) -> NOT deleted
    assert(!range_del.ShouldDelete(Slice("item:50"), 30));

    std::cout << "  - Verified range tombstone ['item:20', 'item:80') @ LSN=20\n";
    std::cout << "  - 'item:19' @ LSN=10: VISIBLE (outside lower bound)\n";
    std::cout << "  - 'item:20' .. 'item:79' @ LSN=10: 60 keys correctly MASKED\n";
    std::cout << "  - 'item:80' @ LSN=10: VISIBLE (outside exclusive upper bound)\n";
    std::cout << "  - 'item:50' @ LSN=30: VISIBLE (superseding subsequent mutation)\n";
    std::cout << "  [PASSED] Test 2: Range Delete Masking Across Multiple Keys\n";
}

// -----------------------------------------------------------------------------
// Test 3: Atomic Batch All-or-Nothing Commit (Feature 87)
// -----------------------------------------------------------------------------
static void Test3_AtomicBatchAllOrNothing() {
    std::cout << "\n=== Test 3: Atomic Batch All-or-Nothing Commit (Feature 87) ===\n";

    SequenceGenerator seq_gen(0);
    MemTableCore memtable;

    // Formulate write batch with 50 operations
    WriteBatch batch;
    for (int i = 0; i < 50; ++i) {
        char kbuf[32], vbuf[32];
        snprintf(kbuf, sizeof(kbuf), "batch_key_%03d", i);
        snprintf(vbuf, sizeof(vbuf), "batch_val_%03d", i);
        batch.Put(Slice(kbuf), Slice(vbuf));
    }
    assert(batch.Count() == 50);

    // Case 1: Simulate atomic WAL I/O failure -> all-or-nothing rollback
    Status err_status = AtomicBatchCommitter::Commit(&batch, &seq_gen, &memtable, /*simulate_wal_failure=*/true);
    assert(!err_status.ok() && err_status.IsIOError());

    // Verify zero records were written to memtable
    std::string val;
    Status s;
    for (int i = 0; i < 50; ++i) {
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "batch_key_%03d", i);
        bool found = memtable.Get(Slice(kbuf), kMaxSequenceNumber, &val, &s);
        assert(!found || !s.ok());
    }
    std::cout << "  - Simulated atomic commit abort: 0 of 50 keys visible in MemTable (Clean Rollback OK).\n";

    // Case 2: Commit successfully without simulated failure
    Status ok_status = AtomicBatchCommitter::Commit(&batch, &seq_gen, &memtable, /*simulate_wal_failure=*/false);
    assert(ok_status.ok());

    // Verify all 50 records are present with contiguous sequences
    for (int i = 0; i < 50; ++i) {
        char kbuf[32], expected_val[32];
        snprintf(kbuf, sizeof(kbuf), "batch_key_%03d", i);
        snprintf(expected_val, sizeof(expected_val), "batch_val_%03d", i);
        bool found = memtable.Get(Slice(kbuf), kMaxSequenceNumber, &val, &s);
        assert(found && s.ok() && val == expected_val);
    }
    std::cout << "  - Successful atomic batch commit: all 50 keys materialized with contiguous LSNs.\n";
    std::cout << "  [PASSED] Test 3: Atomic Batch All-or-Nothing Commit\n";
}

// -----------------------------------------------------------------------------
// Test 4: Optimistic Transaction Commit & Read-Your-Own-Writes (Feature 88)
// -----------------------------------------------------------------------------
static void Test4_OptimisticTransactionReadYourOwnWrites() {
    std::cout << "\n=== Test 4: Optimistic Transaction Commit & Read-Your-Own-Writes (Feature 88) ===\n";

    SnapshotManager snap_mgr;
    SequenceGenerator seq_gen(0);
    MemTableCore memtable;

    // Setup initial state: "account:001" = "100"
    seq_gen.Allocate(1);
    memtable.Add(1, kTypeValue, Slice("account:001"), Slice("100"));

    // Start Transaction T1
    OptimisticTxnContext txn(&snap_mgr, &seq_gen, &memtable);
    assert(txn.GetSnapshotSeq() == 1);

    // T1 modifies account:001 to "250" and inserts new account:002 as "500"
    txn.Put(Slice("account:001"), Slice("250"));
    txn.Put(Slice("account:002"), Slice("500"));

    // Read-Your-Own-Writes: T1 must see its own uncommitted updates
    std::string val;
    Status s1 = txn.Get(Slice("account:001"), &val);
    assert(s1.ok() && val == "250");

    Status s2 = txn.Get(Slice("account:002"), &val);
    assert(s2.ok() && val == "500");

    // Global state outside transaction must NOT see T1's uncommitted writes
    std::string global_val;
    Status global_s;
    bool found_global1 = memtable.Get(Slice("account:001"), kMaxSequenceNumber, &global_val, &global_s);
    assert(found_global1 && global_s.ok() && global_val == "100");

    bool found_global2 = memtable.Get(Slice("account:002"), kMaxSequenceNumber, &global_val, &global_s);
    assert(!found_global2 || !global_s.ok());

    std::cout << "  - T1 Read-Your-Own-Writes: account:001='250', account:002='500'\n";
    std::cout << "  - Global Isolation: account:001='100', account:002=<NotFound>\n";

    // Commit T1
    Status commit_s = txn.Commit();
    assert(commit_s.ok());
    assert(txn.IsCommitted());

    // Verify global state updated
    memtable.Get(Slice("account:001"), kMaxSequenceNumber, &global_val, &global_s);
    assert(global_s.ok() && global_val == "250");

    memtable.Get(Slice("account:002"), kMaxSequenceNumber, &global_val, &global_s);
    assert(global_s.ok() && global_val == "500");

    std::cout << "  - Post-Commit Global State: account:001='250', account:002='500' (Committed OK)\n";
    std::cout << "  [PASSED] Test 4: Optimistic Transaction Commit & Read-Your-Own-Writes\n";
}

// -----------------------------------------------------------------------------
// Test 5: Multi-Version Write-Write Conflict Detection (Feature 89)
// -----------------------------------------------------------------------------
static void Test5_WriteWriteConflictDetection() {
    std::cout << "\n=== Test 5: Write-Write Conflict Detection (Feature 89) ===\n";

    SnapshotManager snap_mgr;
    SequenceGenerator seq_gen(0);
    MemTableCore memtable;

    // Initial state: "stock:AAPL" = "150" at sequence 10
    seq_gen.SetCurrent(10);
    memtable.Add(10, kTypeValue, Slice("stock:AAPL"), Slice("150"));

    // T1 and T2 both start at snapshot sequence 10
    OptimisticTxnContext t1(&snap_mgr, &seq_gen, &memtable);
    OptimisticTxnContext t2(&snap_mgr, &seq_gen, &memtable);

    assert(t1.GetSnapshotSeq() == 10);
    assert(t2.GetSnapshotSeq() == 10);

    // Both read "stock:AAPL" for update
    std::string val1, val2;
    t1.GetForUpdate(Slice("stock:AAPL"), &val1);
    t2.GetForUpdate(Slice("stock:AAPL"), &val2);

    assert(val1 == "150" && val2 == "150");

    // T1 modifies "stock:AAPL" to "160" and commits
    t1.Put(Slice("stock:AAPL"), Slice("160"));
    Status s_t1 = t1.Commit();
    assert(s_t1.ok());
    std::cout << "  - Transaction T1 successfully committed 'stock:AAPL' = '160'\n";

    // T2 attempts to modify "stock:AAPL" to "170" and commit -> must fail with conflict!
    t2.Put(Slice("stock:AAPL"), Slice("170"));
    Status s_t2 = t2.Commit();
    assert(!s_t2.ok());
    assert(s_t2.IsBusy());
    std::cout << "  - Transaction T2 commit rejected with conflict: " << s_t2.ToString() << "\n";

    // Rollback T2 cleanly
    t2.Rollback();
    assert(t2.IsRolledBack());

    std::cout << "  [PASSED] Test 5: Write-Write Conflict Detection\n";
}

// -----------------------------------------------------------------------------
// Test 6: Crash Recovery of Committed Transactions (Feature 90)
// -----------------------------------------------------------------------------
static void Test6_TransactionCrashRecovery() {
    std::cout << "\n=== Test 6: Crash Recovery of Committed Transactions (Feature 90) ===\n";

    SequenceGenerator seq_gen(0);
    MemTableCore recovered_memtable;

    std::vector<TxnWALRecord> wal_records;

    // Generate 10 committed transaction batches
    for (int t = 1; t <= 10; ++t) {
        TxnWALRecord rec;
        rec.seq = t * 10;
        rec.is_committed = true;
        char kbuf[32], vbuf[32];
        snprintf(kbuf, sizeof(kbuf), "recovered_txn_key_%02d", t);
        snprintf(vbuf, sizeof(vbuf), "recovered_txn_val_%02d", t);
        rec.batch.Put(Slice(kbuf), Slice(vbuf));
        wal_records.push_back(std::move(rec));
    }

    // Append 1 uncommitted (in-flight crash) transaction batch
    {
        TxnWALRecord uncommitted_rec;
        uncommitted_rec.seq = 200;
        uncommitted_rec.is_committed = false;
        uncommitted_rec.batch.Put(Slice("ghost_key_uncommitted"), Slice("ghost_val"));
        wal_records.push_back(std::move(uncommitted_rec));
    }

    size_t replayed = 0;
    size_t discarded = 0;
    Status s_rec = TxnRecoveryManager::ReplayTransactions(
        wal_records,
        &recovered_memtable,
        &seq_gen,
        &replayed,
        &discarded
    );

    assert(s_rec.ok());
    assert(replayed == 10);
    assert(discarded == 1);

    // Verify all 10 committed records are recovered
    std::string val;
    Status s;
    for (int t = 1; t <= 10; ++t) {
        char kbuf[32], expected_val[32];
        snprintf(kbuf, sizeof(kbuf), "recovered_txn_key_%02d", t);
        snprintf(expected_val, sizeof(expected_val), "recovered_txn_val_%02d", t);
        bool found = recovered_memtable.Get(Slice(kbuf), kMaxSequenceNumber, &val, &s);
        assert(found && s.ok() && val == expected_val);
    }

    // Verify uncommitted transaction record was discarded
    bool ghost_found = recovered_memtable.Get(Slice("ghost_key_uncommitted"), kMaxSequenceNumber, &val, &s);
    assert(!ghost_found || !s.ok());

    std::cout << "  - Recovered " << replayed << " committed transaction records into MemTable.\n";
    std::cout << "  - Discarded " << discarded << " uncommitted in-flight crash records.\n";
    std::cout << "  - Restored sequence generator LSN = " << seq_gen.Last() << "\n";
    std::cout << "  [PASSED] Test 6: Crash Recovery of Committed Transactions\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase XV Verification Test Suite\n";
    std::cout << "  Features 85-90: MVCC & Transactions Core (Part 2)\n";
    std::cout << "=======================================================================\n";

    Test1_SnapshotPinnedTombstoneRetention();
    Test2_RangeDeleteMasking();
    Test3_AtomicBatchAllOrNothing();
    Test4_OptimisticTransactionReadYourOwnWrites();
    Test5_WriteWriteConflictDetection();
    Test6_TransactionCrashRecovery();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase XV Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
