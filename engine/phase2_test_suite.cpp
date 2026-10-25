#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <thread>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "write_batch.hpp"
#include "memtable.hpp"
#include "iterator.hpp"
#include "memtable_iterator.hpp"
#include "prefix_iterator.hpp"
#include "snapshot.hpp"
#include "transaction.hpp"
#include "ttl_util.hpp"
#include "admin.hpp"
#include "db.hpp"

namespace fs = std::filesystem;
using namespace lsm;

static void CleanTestDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

void test_1_bounded_range_scan() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 1] Feature 7: Bounded Range Scan API [start, end)\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_range");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_range", &db);
    assert(s.ok());

    WriteOptions w_opts;
    ReadOptions r_opts;

    // Insert keys a00 to a99
    for (int i = 0; i < 100; ++i) {
        std::ostringstream ss;
        ss << "a" << std::setw(2) << std::setfill('0') << i;
        std::string k = ss.str();
        std::string v = "val_" + k;
        s = db->Put(w_opts, k, v);
        assert(s.ok());
    }

    // Range Scan [a20, a50) with limit 10
    std::vector<std::pair<std::string, std::string>> results;
    s = db->RangeScan(r_opts, "a20", "a50", 10, &results);
    assert(s.ok());
    std::cout << "  Range scan [a20, a50) limit 10 yielded " << results.size() << " records.\n";
    assert(results.size() == 10);
    assert(results.front().first == "a20");
    assert(results.back().first == "a29");

    // Range Scan [a20, a50) without limit -> returns exactly 30 items
    results.clear();
    s = db->RangeScan(r_opts, "a20", "a50", 0, &results);
    assert(s.ok());
    std::cout << "  Range scan [a20, a50) unlimited yielded " << results.size() << " records.\n";
    assert(results.size() == 30);
    assert(results.front().first == "a20");
    assert(results.back().first == "a49");

    delete db;
    std::cout << ">>> [TEST 1 PASSED]: Bounded range scan boundaries & limits verified.\n";
}

void test_2_bidirectional_iterator() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 2] Feature 8: Bidirectional Iterator API (Seek, Next, Prev)\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_bidi");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_bidi", &db);
    assert(s.ok());

    WriteOptions w_opts;
    ReadOptions r_opts;

    for (int i = 0; i < 100; ++i) {
        std::ostringstream ss;
        ss << "k" << std::setw(3) << std::setfill('0') << i;
        db->Put(w_opts, ss.str(), "v" + std::to_string(i));
    }

    auto iter = db->NewIterator(r_opts);

    // 1. Seek to k050
    iter->Seek("k050");
    assert(iter->Valid());
    assert(iter->key().ToString() == "k050");
    std::cout << "  ✓ Seek('k050') -> " << iter->key().ToString() << "\n";

    // 2. Next() 5 times -> reaches k055
    for (int i = 0; i < 5; ++i) {
        iter->Next();
    }
    assert(iter->Valid());
    assert(iter->key().ToString() == "k055");
    std::cout << "  ✓ Next() x5 -> " << iter->key().ToString() << "\n";

    // 3. Prev() 10 times -> reverses to k045
    for (int i = 0; i < 10; ++i) {
        iter->Prev();
    }
    assert(iter->Valid());
    assert(iter->key().ToString() == "k045");
    std::cout << "  ✓ Prev() x10 -> " << iter->key().ToString() << "\n";

    // 4. SeekToFirst & SeekToLast
    iter->SeekToFirst();
    assert(iter->Valid() && iter->key().ToString() == "k000");
    iter->SeekToLast();
    assert(iter->Valid() && iter->key().ToString() == "k099");
    std::cout << "  ✓ SeekToFirst ('k000') & SeekToLast ('k099') verified.\n";

    delete db;
    std::cout << ">>> [TEST 2 PASSED]: Bidirectional iterator navigation verified.\n";
}

void test_3_consistent_snapshots() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 3] Feature 9: Consistent Point-in-Time Snapshot Isolation\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_snap");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_snap", &db);
    assert(s.ok());

    WriteOptions w_opts;

    // Initial state
    db->Put(w_opts, "account:A", "1000");
    db->Put(w_opts, "account:B", "500");

    // Take Snapshot S1
    const Snapshot* s1 = db->GetSnapshot();
    std::cout << "  ✓ Captured Snapshot S1 at Sequence: " << s1->sequence_number() << "\n";

    // Modify active DB state
    db->Delete(w_opts, "account:A");
    db->Put(w_opts, "account:B", "800");
    db->Put(w_opts, "account:C", "300");

    // Read through Snapshot S1
    ReadOptions s1_opts;
    s1_opts.snapshot = s1->sequence_number();

    std::string val_a, val_b, val_c;
    assert(db->Get(s1_opts, "account:A", &val_a).ok());
    assert(val_a == "1000");

    assert(db->Get(s1_opts, "account:B", &val_b).ok());
    assert(val_b == "500");

    assert(db->Get(s1_opts, "account:C", &val_c).IsNotFound());

    std::cout << "  ✓ Snapshot S1 reads exact historical state: account:A=" << val_a
              << ", account:B=" << val_b << ", account:C=(NotFound)\n";

    // Read through fresh unpinned ReadOptions
    ReadOptions fresh_opts;
    std::string f_val_a, f_val_b, f_val_c;
    assert(db->Get(fresh_opts, "account:A", &f_val_a).IsNotFound());
    assert(db->Get(fresh_opts, "account:B", &f_val_b).ok() && f_val_b == "800");
    assert(db->Get(fresh_opts, "account:C", &f_val_c).ok() && f_val_c == "300");

    std::cout << "  ✓ Fresh reads reflect latest mutations: account:A=(Deleted), account:B="
              << f_val_b << ", account:C=" << f_val_c << "\n";

    db->ReleaseSnapshot(s1);
    delete db;
    std::cout << ">>> [TEST 3 PASSED]: Consistent Snapshot isolation 100% verified.\n";
}

void test_4_optimistic_transactions_occ() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 4] Feature 10: Optimistic ACID Transaction (OCC Conflict Detection)\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_txn");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_txn", &db);
    assert(s.ok());

    WriteOptions w_opts;
    db->Put(w_opts, "inventory:SKU100", "50");

    // Begin Transaction T1
    auto txn1 = db->BeginTransaction();
    std::string stock;
    s = txn1->GetForUpdate(ReadOptions(), "inventory:SKU100", &stock);
    assert(s.ok());
    assert(stock == "50");

    // Mutate in transaction buffer
    txn1->Put("inventory:SKU100", "45");

    // Concurrent external writer commits an out-of-band write to the same key
    db->Put(w_opts, "inventory:SKU100", "40");
    std::cout << "  Out-of-band write modified 'inventory:SKU100' to '40'.\n";

    // Commit T1 -> MUST fail due to write-write conflict on read-set
    s = txn1->Commit();
    std::cout << "  T1 Commit Status: " << s.ToString() << "\n";
    assert(s.code() == Status::kInvalidArgument || s.ToString().find("conflict") != std::string::npos);

    // Rollback T1
    s = txn1->Rollback();
    assert(s.ok());

    // Verify DB retains out-of-band write
    std::string final_stock;
    db->Get(ReadOptions(), "inventory:SKU100", &final_stock);
    assert(final_stock == "40");
    std::cout << "  ✓ Final DB state retains valid concurrent update: " << final_stock << "\n";

    // Non-conflicting transaction T3
    auto txn3 = db->BeginTransaction();
    txn3->Put("inventory:SKU200", "100");
    s = txn3->Commit();
    assert(s.ok());

    std::string sku200;
    db->Get(ReadOptions(), "inventory:SKU200", &sku200);
    assert(sku200 == "100");
    std::cout << "  ✓ Non-conflicting transaction T3 committed successfully.\n";

    txn1.reset();
    txn3.reset();
    delete db;
    std::cout << ">>> [TEST 4 PASSED]: Optimistic Transaction OCC conflict detection verified.\n";
}

void test_5_ttl_expiration() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 5] Feature 11: TTL & Key Expiration Handling\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_ttl");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_ttl", &db);
    assert(s.ok());

    WriteOptions w_opts;
    ReadOptions r_opts;

    // Put with 1-second TTL
    s = db->PutWithTTL(w_opts, "session:token123", "user_auth_payload", 1);
    assert(s.ok());

    // Immediate read -> MUST succeed
    std::string val;
    s = db->Get(r_opts, "session:token123", &val);
    assert(s.ok());
    assert(val == "user_auth_payload");
    std::cout << "  ✓ Immediate lookup before expiration succeeded: " << val << "\n" << std::flush;

    // Sleep 1.5 seconds to cross TTL expiration boundary
    std::cout << "  Sleeping 1500 ms to expire TTL...\n" << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Post-expiration read -> MUST return NotFound
    s = db->Get(r_opts, "session:token123", &val);
    std::cout << "  Post-expiration Get Status: " << s.ToString() << "\n" << std::flush;
    assert(s.IsNotFound());

    delete db;
    std::cout << ">>> [TEST 5 PASSED]: TTL timestamp encoding & expiration filtering verified.\n" << std::flush;
}

void test_6_admin_and_health_diagnostics() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 6] Feature 12: Admin & Engine Health Diagnostic API\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_admin");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_admin", &db);
    assert(s.ok());

    WriteOptions w_opts;
    for (int i = 0; i < 50; ++i) {
        db->Put(w_opts, "diag_k_" + std::to_string(i), "diag_v_" + std::to_string(i));
    }

    const Snapshot* snap = db->GetSnapshot();
    std::string mem_bytes, snap_count, seq_num, num_keys;

    assert(db->GetProperty("lsm.active-memtable-bytes", &mem_bytes));
    assert(db->GetProperty("lsm.active-snapshots", &snap_count));
    assert(db->GetProperty("lsm.sequence-number", &seq_num));
    assert(db->GetProperty("lsm.num-keys", &num_keys));

    std::cout << "  LSM Property 'lsm.active-memtable-bytes': " << mem_bytes << " bytes\n";
    std::cout << "  LSM Property 'lsm.active-snapshots':      " << snap_count << "\n";
    std::cout << "  LSM Property 'lsm.sequence-number':       " << seq_num << "\n";
    std::cout << "  LSM Property 'lsm.num-keys':              " << num_keys << "\n";

    assert(std::stoull(mem_bytes) > 0);
    assert(std::stoul(snap_count) == 1);
    assert(std::stoull(seq_num) == 50);
    assert(std::stoul(num_keys) == 50);

    EngineHealthReport health = db->GetEngineHealth();
    std::cout << "\nEngine Health JSON Report:\n" << health.to_json() << "\n\n";
    assert(health.status == HealthStatus::HEALTHY);
    assert(health.current_sequence == 50);
    assert(health.active_snapshots == 1);

    db->ReleaseSnapshot(snap);
    delete db;
    std::cout << ">>> [TEST 6 PASSED]: Admin properties & health telemetry diagnostics verified.\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << "  LSM-TREE KV ENGINE PHASE II: EXTENDED API, TRANSACTIONS & DIAGNOSTICS\n";
    std::cout << "  Features 7 - 12: RangeScan, BidiIterator, Snapshots, OCC, TTL, Admin \n";
    std::cout << "======================================================================\n";

    test_1_bounded_range_scan();
    test_2_bidirectional_iterator();
    test_3_consistent_snapshots();
    test_4_optimistic_transactions_occ();
    test_5_ttl_expiration();
    test_6_admin_and_health_diagnostics();

    std::cout << "\n======================================================================\n";
    std::cout << "  ALL PHASE II VERIFICATION TESTS PASSED (100% PASS RATE, ZERO ERRORS) \n";
    std::cout << "======================================================================\n";

    return 0;
}
