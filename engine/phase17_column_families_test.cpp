#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>

#include "column_family.hpp"
#include "cf_write_batch.hpp"
#include "cf_write_pipeline.hpp"
#include "cf_flush_controller.hpp"
#include "cf_gc.hpp"
#include "sequence_generator.hpp"
#include "types.hpp"

namespace fs = std::filesystem;
using namespace lsm;

static void SetupDir(const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        fs::remove_all(path, ec);
    }
    fs::create_directories(path, ec);
}

// -----------------------------------------------------------------------------
// Test 1: Dynamic CF Creation & Key Isolation (Features 97 & 98)
// -----------------------------------------------------------------------------
static void Test1_DynamicCFCreationAndIsolation() {
    std::cout << "\n=== Test 1: Dynamic CF Creation & Key Isolation (Features 97 & 98) ===\n";

    ColumnFamilySet cf_set;
    SequenceGenerator seq_gen(0);

    auto h_default = cf_set.GetDefault();
    auto h_users = cf_set.CreateColumnFamily("users");
    auto h_metrics = cf_set.CreateColumnFamily("metrics");

    assert(h_default->GetID() == 0 && h_default->GetName() == "default");
    assert(h_users->GetID() == 1 && h_users->GetName() == "users");
    assert(h_metrics->GetID() == 2 && h_metrics->GetName() == "metrics");

    // Insert identical key "k1" into all 3 column families with different values
    h_default->cfd()->Put(seq_gen.Allocate(1), Slice("k1"), Slice("default_val"));
    h_users->cfd()->Put(seq_gen.Allocate(1), Slice("k1"), Slice("user_alice"));
    h_metrics->cfd()->Put(seq_gen.Allocate(1), Slice("k1"), Slice("123.45"));

    // Verify each column family returns its own isolated value
    std::string val_def, val_users, val_metrics;
    Status s_def = h_default->cfd()->Get(Slice("k1"), kMaxSequenceNumber, &val_def);
    Status s_users = h_users->cfd()->Get(Slice("k1"), kMaxSequenceNumber, &val_users);
    Status s_metrics = h_metrics->cfd()->Get(Slice("k1"), kMaxSequenceNumber, &val_metrics);

    assert(s_def.ok() && val_def == "default_val");
    assert(s_users.ok() && val_users == "user_alice");
    assert(s_metrics.ok() && val_metrics == "123.45");

    std::cout << "  - CF 'default' (ID=0): 'k1' -> '" << val_def << "'\n";
    std::cout << "  - CF 'users'   (ID=1): 'k1' -> '" << val_users << "'\n";
    std::cout << "  - CF 'metrics' (ID=2): 'k1' -> '" << val_metrics << "'\n";
    std::cout << "  - 100% namespace key isolation verified without collision.\n";
    std::cout << "  [PASSED] Test 1: Dynamic CF Creation & Key Isolation\n";
}

// -----------------------------------------------------------------------------
// Test 2: Cross-CF Atomic WriteBatch Commit (Feature 99)
// -----------------------------------------------------------------------------
static void Test2_CrossCFAtomicWriteBatchCommit() {
    std::cout << "\n=== Test 2: Cross-CF Atomic WriteBatch Commit (Feature 99) ===\n";

    ColumnFamilySet cf_set;
    SequenceGenerator seq_gen(0);

    auto h_users = cf_set.CreateColumnFamily("users");
    auto h_accounts = cf_set.CreateColumnFamily("accounts");

    // Case 1: Simulated atomic WAL failure -> all-or-nothing rollback
    CFWriteBatch failed_batch;
    failed_batch.Put(h_users.get(), Slice("user:200"), Slice("Bob"));
    failed_batch.Put(h_accounts.get(), Slice("account:200"), Slice("balance:5000"));

    Status s_err = CFWritePipeline::Write(&failed_batch, &cf_set, &seq_gen, /*simulate_wal_failure=*/true);
    assert(!s_err.ok());

    std::string val;
    Status s1 = h_users->cfd()->Get(Slice("user:200"), kMaxSequenceNumber, &val);
    Status s2 = h_accounts->cfd()->Get(Slice("account:200"), kMaxSequenceNumber, &val);
    assert(!s1.ok() && !s2.ok());
    std::cout << "  - Simulated atomic commit abort: 0 keys materialized across CFs (Clean Rollback OK).\n";

    // Case 2: Successful multi-CF commit
    CFWriteBatch batch;
    batch.Put(h_users.get(), Slice("user:200"), Slice("Bob"));
    batch.Put(h_accounts.get(), Slice("account:200"), Slice("balance:5000"));

    Status s_ok = CFWritePipeline::Write(&batch, &cf_set, &seq_gen, /*simulate_wal_failure=*/false);
    assert(s_ok.ok());

    Status s1_ok = h_users->cfd()->Get(Slice("user:200"), kMaxSequenceNumber, &val);
    assert(s1_ok.ok() && val == "Bob");

    Status s2_ok = h_accounts->cfd()->Get(Slice("account:200"), kMaxSequenceNumber, &val);
    assert(s2_ok.ok() && val == "balance:5000");

    std::cout << "  - Cross-CF atomic batch committed 2 records simultaneously across 'users' and 'accounts'.\n";
    std::cout << "  [PASSED] Test 2: Cross-CF Atomic WriteBatch Commit\n";
}

// -----------------------------------------------------------------------------
// Test 3: Per-CF Independent Flush Execution (Feature 100)
// -----------------------------------------------------------------------------
static void Test3_PerCFIndependentFlushExecution() {
    std::cout << "\n=== Test 3: Per-CF Independent Flush Execution (Feature 100) ===\n";

    std::string test_dir = "./test_phase17_flush";
    SetupDir(test_dir);

    ColumnFamilySet cf_set;
    SequenceGenerator seq_gen(0);

    ColumnFamilyOptions opts_metrics;
    opts_metrics.write_buffer_size = 1024; // Small buffer for testing
    auto h_users = cf_set.CreateColumnFamily("users");
    auto h_metrics = cf_set.CreateColumnFamily("metrics", opts_metrics);

    // Insert 50 records into CF 'metrics'
    for (int i = 0; i < 50; ++i) {
        char kbuf[32], vbuf[32];
        snprintf(kbuf, sizeof(kbuf), "cpu_load_%02d", i);
        snprintf(vbuf, sizeof(vbuf), "0.%02d", i);
        h_metrics->cfd()->Put(seq_gen.Allocate(1), Slice(kbuf), Slice(vbuf));
    }

    // Insert 5 records into CF 'users'
    h_users->cfd()->Put(seq_gen.Allocate(1), Slice("u1"), Slice("Alice"));

    // Flush only CF 'metrics' to SSTable file 101
    Status s_flush = CFFlushController::FlushColumnFamily(test_dir, h_metrics->cfd(), 101, seq_gen.Last());
    assert(s_flush.ok());

    // Verify CF 'metrics' has Level 0 file 101 on disk
    assert(h_metrics->cfd()->GetLevelFiles(0).size() == 1);
    assert(h_metrics->cfd()->GetLevelFiles(0)[0].file_number == 101);

    // Verify CF 'users' remains in memory with zero SSTable files
    assert(h_users->cfd()->GetLevelFiles(0).empty());

    // Verify data remains queryable across both CFs
    std::string val;
    Status s_m = h_metrics->cfd()->Get(Slice("cpu_load_25"), kMaxSequenceNumber, &val);
    assert(s_m.ok() && val == "0.25");

    Status s_u = h_users->cfd()->Get(Slice("u1"), kMaxSequenceNumber, &val);
    assert(s_u.ok() && val == "Alice");

    std::cout << "  - CF 'metrics' flushed to 000101.sst (Level 0 files: 1)\n";
    std::cout << "  - CF 'users' remained in memory (Level 0 files: 0)\n";
    std::cout << "  [PASSED] Test 3: Per-CF Independent Flush Execution\n";
}

// -----------------------------------------------------------------------------
// Test 4: Global WAL Retention Safety (Feature 101)
// -----------------------------------------------------------------------------
static void Test4_GlobalWALRetentionSafety() {
    std::cout << "\n=== Test 4: Global WAL Retention Safety (Feature 101) ===\n";

    ColumnFamilySet cf_set;
    auto h_cf1 = cf_set.CreateColumnFamily("cf1");
    auto h_cf2 = cf_set.CreateColumnFamily("cf2");

    // CF1 flushed up to LSN 500
    h_cf1->cfd()->SetLastFlushedSeq(500);
    // CF2 only flushed up to LSN 200
    h_cf2->cfd()->SetLastFlushedSeq(200);

    // Global min flushed LSN is 200
    SequenceNumber min_flushed = CFFlushController::GetGlobalMinFlushedLSN(&cf_set);
    assert(min_flushed == 200);

    // A WAL file covering sequences up to 300 must NOT be safe to unlink (CF2 still needs it!)
    assert(!CFFlushController::IsWALSafeToUnlink(&cf_set, 300));
    std::cout << "  - WAL segment covering up to LSN 300: RETENTION ENFORCED (CF2 at LSN 200).\n";

    // Now CF2 catches up and flushes up to LSN 550
    h_cf2->cfd()->SetLastFlushedSeq(550);
    min_flushed = CFFlushController::GetGlobalMinFlushedLSN(&cf_set);
    assert(min_flushed == 500);

    // Now WAL up to 300 is safe to unlink
    assert(CFFlushController::IsWALSafeToUnlink(&cf_set, 300));
    std::cout << "  - CF2 flushed past LSN 500: WAL segment up to LSN 300 SAFE TO UNLINK.\n";

    std::cout << "  [PASSED] Test 4: Global WAL Retention Safety\n";
}

// -----------------------------------------------------------------------------
// Test 5: Dropped CF Rejection & Isolation (Feature 102)
// -----------------------------------------------------------------------------
static void Test5_DroppedCFRejectionAndIsolation() {
    std::cout << "\n=== Test 5: Dropped CF Rejection & Isolation (Feature 102) ===\n";

    ColumnFamilySet cf_set;
    SequenceGenerator seq_gen(0);

    auto h_users = cf_set.CreateColumnFamily("users");
    auto h_temp = cf_set.CreateColumnFamily("temp_metrics");

    h_temp->cfd()->Put(seq_gen.Allocate(1), Slice("temp_k"), Slice("temp_v"));
    h_users->cfd()->Put(seq_gen.Allocate(1), Slice("user_k"), Slice("user_v"));

    // Drop CF 'temp_metrics'
    Status s_drop = cf_set.DropColumnFamily(h_temp.get());
    assert(s_drop.ok());
    assert(h_temp->cfd()->IsDropped());

    // Subsequent mutations and queries to dropped CF must be rejected
    Status s_put = h_temp->cfd()->Put(seq_gen.Allocate(1), Slice("k_new"), Slice("v_new"));
    assert(!s_put.ok() && s_put.IsInvalidArgument());

    std::string val;
    Status s_get = h_temp->cfd()->Get(Slice("temp_k"), kMaxSequenceNumber, &val);
    assert(!s_get.ok() && s_get.IsInvalidArgument());

    // Verify other CF ('users') continues normal operation
    Status s_u = h_users->cfd()->Get(Slice("user_k"), kMaxSequenceNumber, &val);
    assert(s_u.ok() && val == "user_v");

    std::cout << "  - Dropped CF operations rejected: " << s_put.ToString() << "\n";
    std::cout << "  - Active CFs unaffected and operational.\n";
    std::cout << "  [PASSED] Test 5: Dropped CF Rejection & Isolation\n";
}

// -----------------------------------------------------------------------------
// Test 6: Asynchronous Resource Reclamation on Drop (Feature 102)
// -----------------------------------------------------------------------------
static void Test6_AsynchronousResourceReclamation() {
    std::cout << "\n=== Test 6: Asynchronous Resource Reclamation on Drop (Feature 102) ===\n";

    std::string test_dir = "./test_phase17_gc";
    SetupDir(test_dir);

    ColumnFamilySet cf_set;
    ColumnFamilyGC gc;

    auto h_audit = cf_set.CreateColumnFamily("audit");
    auto cfd_shared = h_audit->SharedCFD();

    // Create fake SSTable files 201 and 202 on disk for this CF
    std::vector<uint64_t> sst_files = {201, 202};
    for (uint64_t f_num : sst_files) {
        char sst_name[64];
        snprintf(sst_name, sizeof(sst_name), "/%06llu.sst", static_cast<unsigned long long>(f_num));
        std::string path = test_dir + sst_name;
        std::ofstream f(path);
        f << "AUDIT_DATA";
        f.close();
        assert(fs::exists(path));
    }

    // Simulate active reader holding a lease on CF data
    cfd_shared->Ref(); // Ref count > 1

    // Drop column family and register with GC
    cf_set.DropColumnFamily(h_audit.get());
    gc.RegisterDroppedCF(test_dir, cfd_shared, sst_files);
    assert(gc.PendingCount() == 1);

    // First GC pass: Reader is still active -> files must NOT be unlinked
    size_t reclaimed = gc.Scavenge();
    assert(reclaimed == 0);
    assert(fs::exists(test_dir + "/000201.sst"));
    assert(fs::exists(test_dir + "/000202.sst"));
    std::cout << "  - Active reader holding lease: SSTable files PRESERVED during GC pass.\n";

    // Reader finishes and releases reference
    cfd_shared->Unref();

    // Second GC pass: Reader released -> files unlinked and resources reclaimed
    reclaimed = gc.Scavenge();
    assert(reclaimed == 1);
    assert(!fs::exists(test_dir + "/000201.sst"));
    assert(!fs::exists(test_dir + "/000202.sst"));
    assert(gc.PendingCount() == 0);

    std::cout << "  - Reader released lease: 2 SSTable files physically UNLINKED and purged.\n";
    std::cout << "  [PASSED] Test 6: Asynchronous Resource Reclamation on Drop\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase XVII Verification Test Suite\n";
    std::cout << "  Features 97-102: Column Families & Multi-Tenancy Architecture (Part 1)\n";
    std::cout << "=======================================================================\n";

    Test1_DynamicCFCreationAndIsolation();
    Test2_CrossCFAtomicWriteBatchCommit();
    Test3_PerCFIndependentFlushExecution();
    Test4_GlobalWALRetentionSafety();
    Test5_DroppedCFRejectionAndIsolation();
    Test6_AsynchronousResourceReclamation();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase XVII Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
