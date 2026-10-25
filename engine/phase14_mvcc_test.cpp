#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <thread>
#include <chrono>
#include <memory>
#include <algorithm>
#include <unordered_set>
#include <filesystem>

#include "sequence_generator.hpp"
#include "internal_key.hpp"
#include "snapshot_manager.hpp"
#include "visibility_filter.hpp"
#include "mvcc_resolver.hpp"
#include "memtable_core.hpp"
#include "memtable_iter.hpp"
#include "table_builder.hpp"
#include "table_reader.hpp"
#include "merging_iterator.hpp"
#include "types.hpp"

namespace fs = std::filesystem;
using namespace lsm;

// -----------------------------------------------------------------------------
// Test 1: Monotonic Sequence Generation (Feature 79)
// -----------------------------------------------------------------------------
static void Test1_MonotonicSequenceGeneration() {
    std::cout << "\n=== Test 1: Monotonic Sequence Generation (Feature 79) ===\n";
    SequenceGenerator seq_gen(0);
    const int num_threads = 16;
    const int allocs_per_thread = 10000;
    const int total_allocs = num_threads * allocs_per_thread;

    std::vector<std::vector<SequenceNumber>> thread_results(num_threads);
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([t, &seq_gen, &thread_results]() {
            thread_results[t].reserve(allocs_per_thread);
            for (int i = 0; i < allocs_per_thread; ++i) {
                SequenceNumber s = seq_gen.Allocate(1);
                thread_results[t].push_back(s);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    std::vector<SequenceNumber> all_sequences;
    all_sequences.reserve(total_allocs);
    for (int t = 0; t < num_threads; ++t) {
        for (SequenceNumber s : thread_results[t]) {
            all_sequences.push_back(s);
        }
    }

    assert(static_cast<int>(all_sequences.size()) == total_allocs);
    std::sort(all_sequences.begin(), all_sequences.end());

    // Verify strictly monotonic [1 .. total_allocs] with zero duplicates and zero gaps
    for (size_t i = 0; i < all_sequences.size(); ++i) {
        assert(all_sequences[i] == (i + 1));
    }

    // Verify recovery SetCurrent monotonic catch-up
    seq_gen.SetCurrent(500000);
    assert(seq_gen.Last() == 500000);
    SequenceNumber next_s = seq_gen.Allocate(5);
    assert(next_s == 500001);
    assert(seq_gen.Last() == 500005);

    std::cout << "  - Concurrently allocated " << total_allocs << " sequence numbers across "
              << num_threads << " threads.\n";
    std::cout << "  - Verified zero duplicates, zero gaps: strictly monotonic [1 .. "
              << total_allocs << "].\n";
    std::cout << "  [PASSED] Test 1: Monotonic Sequence Generation\n";
}

// -----------------------------------------------------------------------------
// Test 2: InternalKey Encoding & Reverse Sequence Sorting (Feature 80)
// -----------------------------------------------------------------------------
static void Test2_InternalKeyEncodingAndSorting() {
    std::cout << "\n=== Test 2: InternalKey Encoding & Reverse Sequence Sorting (Feature 80) ===\n";

    InternalKey key_seq10("user_alice", 10, kTypeValue);
    InternalKey key_seq20("user_alice", 20, kTypeValue);
    InternalKey key_seq30("user_alice", 30, kTypeValue);
    InternalKey key_seq30_del("user_alice", 30, kTypeDeletion);
    InternalKey key_bob("user_bob", 5, kTypeValue);

    // Verify internal key parsing
    ParsedInternalKey parsed;
    bool ok = ParseInternalKey(key_seq30.Encode(), &parsed);
    assert(ok);
    assert(parsed.user_key.ToString() == "user_alice");
    assert(parsed.sequence == 30);
    assert(parsed.type == kTypeValue);

    // Verify comparator: user_alice comes before user_bob
    InternalKeyComparatorImpl cmp;
    assert(cmp.Compare(key_seq10, key_bob) < 0);
    assert(cmp.Compare(key_bob, key_seq10) > 0);

    // For identical user keys: higher sequence numbers MUST sort before lower sequence numbers
    assert(cmp.Compare(key_seq30, key_seq20) < 0); // 30 comes BEFORE 20
    assert(cmp.Compare(key_seq20, key_seq10) < 0); // 20 comes BEFORE 10
    assert(cmp.Compare(key_seq30, key_seq10) < 0); // 30 comes BEFORE 10

    // Test vector sorting
    std::vector<InternalKey> keys = {key_seq10, key_seq30, key_bob, key_seq20};
    std::sort(keys.begin(), keys.end(), [&cmp](const InternalKey& a, const InternalKey& b) {
        return cmp.Compare(a, b) < 0;
    });

    assert(keys[0].user_key().ToString() == "user_alice" && keys[0].sequence() == 30);
    assert(keys[1].user_key().ToString() == "user_alice" && keys[1].sequence() == 20);
    assert(keys[2].user_key().ToString() == "user_alice" && keys[2].sequence() == 10);
    assert(keys[3].user_key().ToString() == "user_bob" && keys[3].sequence() == 5);

    // Verify LookupKey construction
    LookupKey lkey("user_alice", 20);
    assert(lkey.user_key().ToString() == "user_alice");

    std::cout << "  - Verified reverse sequence sorting: 30 < 20 < 10 for same user key.\n";
    std::cout << "  - Verified debug string: " << DebugString(key_seq30.Encode()) << "\n";
    std::cout << "  [PASSED] Test 2: InternalKey Encoding & Reverse Sequence Sorting\n";
}

// -----------------------------------------------------------------------------
// Test 3: Point-in-Time Snapshot Isolation (Feature 81)
// -----------------------------------------------------------------------------
static void Test3_PointInTimeSnapshotIsolation() {
    std::cout << "\n=== Test 3: Point-in-Time Snapshot Isolation (Feature 81) ===\n";

    SequenceGenerator seq_gen(0);
    SnapshotManager snap_mgr;
    MemTableCore memtable;

    // Step 1: Write "k1" = "v1" at seq 1
    SequenceNumber s1_seq = seq_gen.Allocate(1);
    memtable.Add(s1_seq, kTypeValue, Slice("k1"), Slice("v1"));
    const Snapshot* snap1 = snap_mgr.CreateSnapshot(s1_seq);

    // Step 2: Update "k1" = "v2" at seq 2
    SequenceNumber s2_seq = seq_gen.Allocate(1);
    memtable.Add(s2_seq, kTypeValue, Slice("k1"), Slice("v2"));
    const Snapshot* snap2 = snap_mgr.CreateSnapshot(s2_seq);

    // Step 3: Delete "k1" at seq 3
    SequenceNumber s3_seq = seq_gen.Allocate(1);
    memtable.Add(s3_seq, kTypeDeletion, Slice("k1"), Slice(""));

    // Step 4: Verify reads at snap1, snap2, and current (seq 3)
    std::string val;
    Status s;

    // Read at Snap1 (seq 1) -> must see "v1"
    bool found1 = memtable.Get(Slice("k1"), snap1->GetSequenceNumber(), &val, &s);
    assert(found1 && s.ok());
    assert(val == "v1");

    // Read at Snap2 (seq 2) -> must see "v2"
    bool found2 = memtable.Get(Slice("k1"), snap2->GetSequenceNumber(), &val, &s);
    assert(found2 && s.ok());
    assert(val == "v2");

    // Read at current seq 3 (deleted) -> must return NotFound
    bool found3 = memtable.Get(Slice("k1"), s3_seq, &val, &s);
    assert(found3 && s.IsNotFound());

    // Release snapshots
    snap_mgr.ReleaseSnapshot(snap1);
    snap_mgr.ReleaseSnapshot(snap2);
    assert(snap_mgr.IsEmpty());

    std::cout << "  - Snapshot 1 (LSN=" << s1_seq << "): read 'v1' (OK)\n";
    std::cout << "  - Snapshot 2 (LSN=" << s2_seq << "): read 'v2' (OK)\n";
    std::cout << "  - Current Read (LSN=" << s3_seq << "): read Status::NotFound (OK)\n";
    std::cout << "  [PASSED] Test 3: Point-in-Time Snapshot Isolation\n";
}

// -----------------------------------------------------------------------------
// Test 4: Snapshot Visibility Boundary (Feature 82)
// -----------------------------------------------------------------------------
static void Test4_SnapshotVisibilityBoundary() {
    std::cout << "\n=== Test 4: Snapshot Visibility Boundary (Feature 82) ===\n";

    MemTableCore memtable;
    memtable.Add(100, kTypeValue, Slice("boundary_key"), Slice("val_100"));
    memtable.Add(200, kTypeValue, Slice("boundary_key"), Slice("val_200"));
    memtable.Add(300, kTypeValue, Slice("boundary_key"), Slice("val_300"));

    std::string val;
    Status s;

    // Snapshot at seq 99 -> key not yet created
    bool found_99 = memtable.Get(Slice("boundary_key"), 99, &val, &s);
    assert(!found_99 || s.IsNotFound());

    // Snapshot at seq 100 -> sees val_100
    bool found_100 = memtable.Get(Slice("boundary_key"), 100, &val, &s);
    assert(found_100 && s.ok() && val == "val_100");

    // Snapshot at seq 199 -> sees val_100
    bool found_199 = memtable.Get(Slice("boundary_key"), 199, &val, &s);
    assert(found_199 && s.ok() && val == "val_100");

    // Snapshot at seq 200 -> sees val_200
    bool found_200 = memtable.Get(Slice("boundary_key"), 200, &val, &s);
    assert(found_200 && s.ok() && val == "val_200");

    // Snapshot at seq 300 -> sees val_300
    bool found_300 = memtable.Get(Slice("boundary_key"), 300, &val, &s);
    assert(found_300 && s.ok() && val == "val_300");

    // Verify inlined VisibilityFilter predicates
    assert(VisibilityFilter::IsVisible(100, 199));
    assert(!VisibilityFilter::IsVisible(200, 199));
    assert(VisibilityFilter::IsInvisibleFutureWrite(200, 199));

    std::cout << "  - LSN 99: NotFound (invisible future writes)\n";
    std::cout << "  - LSN 100 & 199: correctly clamped to 'val_100'\n";
    std::cout << "  - LSN 200: correctly advanced to 'val_200'\n";
    std::cout << "  - LSN 300: correctly advanced to 'val_300'\n";
    std::cout << "  [PASSED] Test 4: Snapshot Visibility Boundary\n";
}

// -----------------------------------------------------------------------------
// Test 5: Stale Snapshot Expiration & Sweep (Feature 83)
// -----------------------------------------------------------------------------
static void Test5_StaleSnapshotExpirationAndSweep() {
    std::cout << "\n=== Test 5: Stale Snapshot Expiration & Sweep (Feature 83) ===\n";

    SnapshotManager snap_mgr;

    // Create 3 snapshots: s1 (seq 10), s2 (seq 20), s3 (seq 30)
    const Snapshot* s1 = snap_mgr.CreateSnapshot(10);
    const Snapshot* s2 = snap_mgr.CreateSnapshot(20);

    assert(snap_mgr.Count() == 2);
    assert(snap_mgr.GetOldestSnapshot() == 10);

    // Sleep 60ms to let s1 and s2 age past 50ms TTL
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Create fresh snapshot s3 (seq 30)
    const Snapshot* s3 = snap_mgr.CreateSnapshot(30);
    (void)s1; (void)s2; // Handles swept below

    assert(snap_mgr.Count() == 3);

    // Sweep snapshots older than 50ms (50,000,000 ns)
    std::vector<SequenceNumber> swept_seqs;
    size_t swept_count = snap_mgr.SweepExpiredSnapshots(50 * 1000 * 1000ULL, &swept_seqs);

    assert(swept_count == 2);
    assert(swept_seqs.size() == 2);
    assert(swept_seqs[0] == 10 && swept_seqs[1] == 20);

    // Oldest active snapshot is now s3 (seq 30)
    assert(snap_mgr.Count() == 1);
    assert(snap_mgr.GetOldestSnapshot() == 30);

    // Clean up remaining s3
    snap_mgr.ReleaseSnapshot(s3);
    assert(snap_mgr.IsEmpty());
    assert(snap_mgr.GetOldestSnapshot(999) == 999);

    std::cout << "  - Successfully swept 2 expired snapshots (LSN 10, LSN 20).\n";
    std::cout << "  - Advanced oldest active snapshot sequence from 10 to 30 to unblock GC.\n";
    std::cout << "  [PASSED] Test 5: Stale Snapshot Expiration & Sweep\n";
}

// -----------------------------------------------------------------------------
// Test 6: MVCC Tombstone Shadow Masking & Multi-Version Resolution (Feature 84)
// -----------------------------------------------------------------------------
static void Test6_MVCCResolutionAndTombstoneMasking() {
    std::cout << "\n=== Test 6: MVCC Tombstone Shadow Masking (Feature 84) ===\n";

    MemTableCore memtable;
    // 5 updates followed by a deletion
    memtable.Add(10, kTypeValue, Slice("account:101"), Slice("balance:100"));
    memtable.Add(20, kTypeValue, Slice("account:101"), Slice("balance:250"));
    memtable.Add(30, kTypeValue, Slice("account:101"), Slice("balance:500"));
    memtable.Add(40, kTypeValue, Slice("account:101"), Slice("balance:750"));
    memtable.Add(50, kTypeValue, Slice("account:101"), Slice("balance:1000"));
    memtable.Add(60, kTypeDeletion, Slice("account:101"), Slice(""));

    MemTableInternalIterator internal_iter(memtable.NewIterator());

    std::string val;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;

    // Test resolution at each snapshot
    Status s1 = MVCCResolver::ResolvePointLookup(&internal_iter, Slice("account:101"), 15, &val, &seq, &type);
    assert(s1.ok() && val == "balance:100" && seq == 10);

    Status s2 = MVCCResolver::ResolvePointLookup(&internal_iter, Slice("account:101"), 35, &val, &seq, &type);
    assert(s2.ok() && val == "balance:500" && seq == 30);

    Status s3 = MVCCResolver::ResolvePointLookup(&internal_iter, Slice("account:101"), 55, &val, &seq, &type);
    assert(s3.ok() && val == "balance:1000" && seq == 50);

    Status s4 = MVCCResolver::ResolvePointLookup(&internal_iter, Slice("account:101"), 65, &val, &seq, &type);
    assert(s4.IsNotFound() && type == kTypeDeletion);

    // Test full history extraction
    std::vector<std::pair<SequenceNumber, std::string>> history;
    Status s_hist = MVCCResolver::ResolveMultiVersionHistory(&internal_iter, Slice("account:101"), &history);
    assert(s_hist.ok());
    assert(history.size() == 6);
    assert(history[0].first == 60 && history[0].second == "<TOMBSTONE_DELETED>");
    assert(history[1].first == 50 && history[1].second == "balance:1000");
    assert(history[2].first == 40 && history[2].second == "balance:750");
    assert(history[3].first == 30 && history[3].second == "balance:500");
    assert(history[4].first == 20 && history[4].second == "balance:250");
    assert(history[5].first == 10 && history[5].second == "balance:100");

    std::cout << "  - Extracted full version history (" << history.size() << " versions in descending LSN order):\n";
    for (const auto& entry : history) {
        std::cout << "    * LSN " << entry.first << " -> " << entry.second << "\n";
    }

    assert(MVCCResolver::IsKeyDeletedAtSnapshot(&internal_iter, Slice("account:101"), 65));
    assert(!MVCCResolver::IsKeyDeletedAtSnapshot(&internal_iter, Slice("account:101"), 55));

    std::cout << "  [PASSED] Test 6: MVCC Tombstone Shadow Masking\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase XIV Verification Test Suite\n";
    std::cout << "  Features 79-84: MVCC & Transactions Core (Part 1)\n";
    std::cout << "=======================================================================\n";

    Test1_MonotonicSequenceGeneration();
    Test2_InternalKeyEncodingAndSorting();
    Test3_PointInTimeSnapshotIsolation();
    Test4_SnapshotVisibilityBoundary();
    Test5_StaleSnapshotExpirationAndSweep();
    Test6_MVCCResolutionAndTombstoneMasking();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase XIV Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
