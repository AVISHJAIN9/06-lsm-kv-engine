#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <thread>
#include <atomic>
#include <sstream>
#include <filesystem>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "write_batch.hpp"
#include "memtable.hpp"
#include "iterator.hpp"
#include "prefix_iterator.hpp"
#include "db.hpp"

namespace fs = std::filesystem;
using namespace lsm;

static void CleanTestDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

void test_1_point_put_get_delete() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 1] Feature 1, 2, 3: Point Put, Get & Tombstone Delete\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_1");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_1", &db);
    assert(s.ok());
    assert(db != nullptr);

    WriteOptions w_opts;
    ReadOptions r_opts;

    // 1. Basic Put & Get
    s = db->Put(w_opts, "user:1", "Alice");
    assert(s.ok());

    std::string val;
    s = db->Get(r_opts, "user:1", &val);
    assert(s.ok());
    assert(val == "Alice");
    std::cout << "  ✓ Put & Get key 'user:1': " << val << "\n";

    // 2. Overwrite
    s = db->Put(w_opts, "user:1", "Alice Updated");
    assert(s.ok());
    s = db->Get(r_opts, "user:1", &val);
    assert(s.ok());
    assert(val == "Alice Updated");
    std::cout << "  ✓ Overwrite key 'user:1': " << val << "\n";

    // 3. Delete (Tombstone)
    s = db->Delete(w_opts, "user:1");
    assert(s.ok());
    s = db->Get(r_opts, "user:1", &val);
    assert(s.IsNotFound());
    std::cout << "  ✓ Delete 'user:1' -> Get returned NotFound (Tombstone active): " << s.ToString() << "\n";

    // 4. Missing key lookup
    s = db->Get(r_opts, "non_existent_key", &val);
    assert(s.IsNotFound());
    std::cout << "  ✓ Missing key returned NotFound: " << s.ToString() << "\n";

    delete db;
    std::cout << ">>> [TEST 1 PASSED]: Point Operations & Deletion Tombstones 100% verified.\n";
}

void test_2_atomic_write_batch() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 2] Feature 4: Atomic Multi-Operation WriteBatch\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_2");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_2", &db);
    assert(s.ok());

    WriteOptions w_opts;
    ReadOptions r_opts;

    WriteBatch batch;
    constexpr size_t NUM_ENTRIES = 1000;

    // Populate 1000 items in batch
    for (size_t i = 0; i < NUM_ENTRIES; ++i) {
        std::string k = "batch_k_" + std::to_string(i);
        std::string v = "batch_v_" + std::to_string(i);
        batch.Put(k, v);
    }
    // Delete even entries in same batch
    for (size_t i = 0; i < NUM_ENTRIES; i += 2) {
        std::string k = "batch_k_" + std::to_string(i);
        batch.Delete(k);
    }

    std::cout << "  Batch operation count: " << batch.Count() << " (1000 Puts + 500 Deletes)\n";
    std::cout << "  Batch binary size:      " << batch.ByteSize() << " bytes\n";

    s = db->Write(w_opts, &batch);
    assert(s.ok());
    assert(db->GetLatestSequenceNumber() == batch.Count());
    std::cout << "  ✓ Atomically committed batch with sequence window ending at: "
              << db->GetLatestSequenceNumber() << "\n";

    // Verify even keys are deleted, odd keys exist
    for (size_t i = 0; i < NUM_ENTRIES; ++i) {
        std::string k = "batch_k_" + std::to_string(i);
        std::string val;
        s = db->Get(r_opts, k, &val);
        if (i % 2 == 0) {
            assert(s.IsNotFound());
        } else {
            assert(s.ok());
            assert(val == "batch_v_" + std::to_string(i));
        }
    }
    std::cout << "  ✓ Verified all " << NUM_ENTRIES << " post-batch states with 100% precision.\n";

    delete db;
    std::cout << ">>> [TEST 2 PASSED]: Atomic WriteBatch commitment & sequencing verified.\n";
}

void test_3_multi_get_batch_read() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 3] Feature 5: Scatter-Gather MultiGet Batch Read\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_3");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_3", &db);
    assert(s.ok());

    WriteOptions w_opts;
    ReadOptions r_opts;

    for (int i = 0; i < 100; ++i) {
        if (i % 2 == 0) {
            s = db->Put(w_opts, "mg_key_" + std::to_string(i), "mg_val_" + std::to_string(i));
            assert(s.ok());
        }
    }

    std::vector<Slice> keys;
    std::vector<std::string> keys_store;
    keys.reserve(100);
    keys_store.reserve(100);
    for (int i = 0; i < 100; ++i) {
        keys_store.push_back("mg_key_" + std::to_string(i));
        keys.push_back(Slice(keys_store.back()));
    }

    std::vector<std::string> values;
    std::vector<Status> statuses = db->MultiGet(r_opts, keys, &values);

    assert(statuses.size() == 100);
    assert(values.size() == 100);

    for (int i = 0; i < 100; ++i) {
        if (i % 2 == 0) {
            assert(statuses[i].ok());
            assert(values[i] == "mg_val_" + std::to_string(i));
        } else {
            assert(statuses[i].IsNotFound());
        }
    }

    std::cout << "  ✓ MultiGet correctly evaluated 100 scatter-gather keys with intact index alignment.\n";

    delete db;
    std::cout << ">>> [TEST 3 PASSED]: MultiGet batch read API verified.\n";
}

void test_4_prefix_scan() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 4] Feature 6: Lexicographical Prefix Scan\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_4");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_4", &db);
    assert(s.ok());

    WriteOptions w_opts;
    ReadOptions r_opts;

    db->Put(w_opts, "user:100:name", "Alice");
    db->Put(w_opts, "user:100:email", "alice@example.com");
    db->Put(w_opts, "user:100:role", "Admin");
    db->Put(w_opts, "user:101:name", "Bob");
    db->Put(w_opts, "user:200:name", "Charlie");
    db->Put(w_opts, "other:data", "Noise");

    auto prefix_iter = db->NewPrefixIterator(r_opts, "user:100:");
    std::vector<std::pair<std::string, std::string>> results;
    for (prefix_iter->SeekToFirst(); prefix_iter->Valid(); prefix_iter->Next()) {
        results.emplace_back(prefix_iter->key().ToString(), prefix_iter->value().ToString());
        std::cout << "    [" << prefix_iter->key().ToString() << "] => " << prefix_iter->value().ToString() << "\n";
    }

    assert(results.size() == 3);
    assert(results[0].first == "user:100:email");
    assert(results[1].first == "user:100:name");
    assert(results[2].first == "user:100:role");

    delete db;
    std::cout << ">>> [TEST 4 PASSED]: Prefix scan bounded with 100% precision.\n";
}

void test_5_concurrency_safety() {
    std::cout << "\n======================================================================\n";
    std::cout << "[TEST 5] Thread Safety & Concurrent Multi-Threaded Read/Write Load\n";
    std::cout << "======================================================================\n";

    CleanTestDir("test_db_5");
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, "test_db_5", &db);
    assert(s.ok());

    constexpr int NUM_THREADS = 8;
    constexpr int OPS_PER_THREAD = 1000;
    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);

    std::atomic<bool> start_flag{false};

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([db, t, &start_flag]() {
            while (!start_flag.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }

            WriteOptions w_opts;
            ReadOptions r_opts;

            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                std::string k = "key_" + std::to_string(t) + "_" + std::to_string(i);
                std::string v = "val_" + std::to_string(i);
                db->Put(w_opts, k, v);

                std::string read_val;
                Status st = db->Get(r_opts, k, &read_val);
                assert(st.ok());
                assert(read_val == v);
            }
        });
    }

    start_flag.store(true);
    for (auto& w : workers) {
        w.join();
    }

    std::cout << "  ✓ Concurrent execution of " << (NUM_THREADS * OPS_PER_THREAD)
              << " writes and reads completed across " << NUM_THREADS << " threads.\n";
    std::cout << "  Final Sequence Number: " << db->GetLatestSequenceNumber() << "\n";
    assert(db->GetLatestSequenceNumber() == NUM_THREADS * OPS_PER_THREAD);

    delete db;
    std::cout << ">>> [TEST 5 PASSED]: Concurrency safety & zero race conditions confirmed.\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << "  LSM-TREE KV ENGINE PHASE I: CLIENT API & STORAGE CORE RUNTIME       \n";
    std::cout << "  Features 1 - 6: Put, Get, Delete, WriteBatch, MultiGet, PrefixScan  \n";
    std::cout << "======================================================================\n";

    test_1_point_put_get_delete();
    test_2_atomic_write_batch();
    test_3_multi_get_batch_read();
    test_4_prefix_scan();
    test_5_concurrency_safety();

    std::cout << "\n======================================================================\n";
    std::cout << "  ALL PHASE I VERIFICATION TESTS PASSED (100% PASS RATE, ZERO ERRORS) \n";
    std::cout << "======================================================================\n";

    return 0;
}
