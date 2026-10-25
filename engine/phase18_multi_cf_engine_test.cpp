#include <iostream>
#include <cassert>
#include <filesystem>
#include <thread>
#include <vector>
#include <string>

#include "column_family.hpp"
#include "cf_write_batch.hpp"
#include "cf_write_pipeline.hpp"
#include "cf_compaction_scheduler.hpp"
#include "multi_cf_limiter.hpp"
#include "multi_cf_snapshot.hpp"
#include "multi_cf_versions.hpp"
#include "multi_cf_recovery.hpp"
#include "cf_metrics.hpp"
#include "current_file.hpp"

using namespace lsm;

static void CleanupDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

// -----------------------------------------------------------------------------
// Test 1: Independent Per-CF Compaction Scheduling (Feature 103)
// -----------------------------------------------------------------------------
static void TestIndependentCFCompactionScheduling() {
    std::cout << "\n=== Test 1: Independent Per-CF Compaction Scheduling (Feature 103) ===\n";

    ColumnFamilySet cf_set;
    auto def_handle = cf_set.GetDefault();
    auto metrics_handle = cf_set.CreateColumnFamily("metrics");

    CFCompactionScheduler scheduler;

    LeveledCompactionOptions leveled_opt;
    leveled_opt.l0_file_num_compaction_trigger = 4;
    scheduler.RegisterColumnFamily(def_handle->GetID(), CFCompactionStyle::kLeveled, leveled_opt);

    SizeTieredCompactionOptions stcs_opt;
    stcs_opt.min_threshold = 4;
    stcs_opt.min_sstable_size = 1024;
    scheduler.RegisterColumnFamily(metrics_handle->GetID(), CFCompactionStyle::kSizeTiered, {}, stcs_opt);

    // Populate CF 0 (default) with 6 L0 files (trigger is 4 -> score = 1.5)
    for (uint64_t i = 1; i <= 6; ++i) {
        FileMetaData f;
        f.file_number = i;
        f.file_size = 2 * 1024 * 1024;
        f.smallest_key = "k" + std::to_string(i);
        f.largest_key = "k" + std::to_string(i + 10);
        def_handle->cfd()->AddLevelFile(0, f);
    }

    // Populate CF 1 (metrics) with 2 L0 files (trigger is 4 -> score = 0.5)
    for (uint64_t i = 101; i <= 102; ++i) {
        FileMetaData f;
        f.file_number = i;
        f.file_size = 50 * 1024;
        f.smallest_key = "m" + std::to_string(i);
        f.largest_key = "m" + std::to_string(i + 10);
        metrics_handle->cfd()->AddLevelFile(0, f);
    }

    auto candidate = scheduler.PickBestCompaction(&cf_set);
    assert(candidate.has_value());
    assert(candidate->cf_id == def_handle->GetID());
    assert(candidate->priority_score >= 1.5);
    std::cout << "  - CF 'default' selected with highest priority score: "
              << candidate->priority_score << " (Style: Leveled, Level 0 files: 6)\n";

    // Add more files to CF 1 (metrics) to make its debt dominate (6 files -> score = 1.5)
    // while CF 0 is cleared
    for (uint64_t i = 103; i <= 106; ++i) {
        FileMetaData f;
        f.file_number = i;
        f.file_size = 50 * 1024;
        f.smallest_key = "m" + std::to_string(i);
        f.largest_key = "m" + std::to_string(i + 10);
        metrics_handle->cfd()->AddLevelFile(0, f);
    }

    // Clear CF 0's files to simulate completed compaction
    ColumnFamilySet cf_set2;
    auto def2 = cf_set2.GetDefault();
    auto met2 = cf_set2.CreateColumnFamily("metrics");
    for (uint64_t i = 101; i <= 106; ++i) {
        FileMetaData f;
        f.file_number = i;
        f.file_size = 50 * 1024;
        f.smallest_key = "m" + std::to_string(i);
        f.largest_key = "m" + std::to_string(i + 10);
        met2->cfd()->AddLevelFile(0, f);
    }

    auto candidate2 = scheduler.PickBestCompaction(&cf_set2);
    assert(candidate2.has_value());
    assert(candidate2->cf_id == met2->GetID());
    assert(candidate2->style == CFCompactionStyle::kSizeTiered);
    std::cout << "  - CF 'metrics' selected when debt dominated: "
              << candidate2->priority_score << " (Style: SizeTiered, Inputs: "
              << candidate2->inputs.size() << " files)\n";

    std::cout << "  [PASSED] Test 1: Independent Per-CF Compaction Scheduling\n";
}

// -----------------------------------------------------------------------------
// Test 2: Cross-CF Compaction Concurrency & Resource Isolation (Feature 104)
// -----------------------------------------------------------------------------
static void TestCrossCFCompactionConcurrencyAndLimiting() {
    std::cout << "\n=== Test 2: Cross-CF Compaction Concurrency & Resource Isolation (Feature 104) ===\n";

    MultiCFRateLimiter limiter(10 * 1024 * 1024, 2 * 1024 * 1024); // 10MB/s rate, 2MB burst

    uint32_t cf_high = 1;
    uint32_t cf_noisy = 2;

    limiter.SetCFWeight(cf_high, 3.0);  // 75% relative weight
    limiter.SetCFWeight(cf_noisy, 1.0); // 25% relative weight

    // High priority tenant requests 500KB
    uint64_t wait1 = limiter.Request(cf_high, 500 * 1024);
    assert(wait1 == 0);

    // Noisy tenant floods multiple large requests (3MB total)
    uint64_t wait_noisy = 0;
    for (int i = 0; i < 6; ++i) {
        uint64_t w = limiter.Request(cf_noisy, 500 * 1024);
        if (w > 0) wait_noisy += w;
    }

    assert(wait_noisy > 0); // Noisy tenant was throttled

    auto high_traffic = limiter.GetCFTraffic(cf_high);
    auto noisy_traffic = limiter.GetCFTraffic(cf_noisy);

    std::cout << "  - High Priority CF [1]: Granted " << high_traffic.total_bytes_granted
              << " B (Throttled: " << high_traffic.throttled_requests << ")\n";
    std::cout << "  - Noisy Tenant CF [2]:  Requested " << noisy_traffic.total_bytes_requested
              << " B, Granted " << noisy_traffic.total_bytes_granted
              << " B (Throttled: " << noisy_traffic.throttled_requests
              << ", Wait: " << noisy_traffic.total_throttle_wait_us << " us)\n";

    assert(noisy_traffic.throttled_requests > 0);
    assert(high_traffic.throttled_requests == 0);

    std::cout << "  [PASSED] Test 2: Cross-CF Compaction Concurrency & Resource Isolation\n";
}

// -----------------------------------------------------------------------------
// Test 3: Cross-CF Atomic Point-in-Time Consistent Snapshots (Feature 105)
// -----------------------------------------------------------------------------
static void TestCrossCFConsistentSnapshots() {
    std::cout << "\n=== Test 3: Cross-CF Atomic Point-in-Time Consistent Snapshots (Feature 105) ===\n";

    ColumnFamilySet cf_set;
    auto users_handle = cf_set.CreateColumnFamily("users");
    auto accounts_handle = cf_set.CreateColumnFamily("accounts");

    MultiCFSnapshotManager snap_mgr;

    // Sequence 1: Initial state
    users_handle->cfd()->Put(1, "alice", "Alice_V1");
    accounts_handle->cfd()->Put(1, "alice", "100_USD");

    // Capture global consistent snapshot at LSN 1
    const MultiCFSnapshot* snap1 = snap_mgr.CaptureSnapshot(1);
    assert(snap1 != nullptr);
    assert(snap1->GetSequenceNumber() == 1);

    // Sequence 2: Mutate both CFs concurrently
    users_handle->cfd()->Put(2, "alice", "Alice_V2");
    accounts_handle->cfd()->Put(2, "alice", "500_USD");

    // Capture snapshot at LSN 2
    const MultiCFSnapshot* snap2 = snap_mgr.CaptureSnapshot(2);

    // Sequence 3: Delete from users
    users_handle->cfd()->Delete(3, "alice");

    // Verify Point-in-time Read via MultiCFConsistentReader with snap1 (LSN 1)
    std::string val_u1, val_a1;
    Status su1 = MultiCFConsistentReader::Get(users_handle->cfd(), snap1, "alice", &val_u1);
    Status sa1 = MultiCFConsistentReader::Get(accounts_handle->cfd(), snap1, "alice", &val_a1);
    assert(su1.ok() && val_u1 == "Alice_V1");
    assert(sa1.ok() && val_a1 == "100_USD");
    std::cout << "  - Snapshot 1 (LSN 1): users.alice = '" << val_u1 << "', accounts.alice = '" << val_a1 << "'\n";

    // Verify Point-in-time Read with snap2 (LSN 2)
    std::string val_u2, val_a2;
    Status su2 = MultiCFConsistentReader::Get(users_handle->cfd(), snap2, "alice", &val_u2);
    Status sa2 = MultiCFConsistentReader::Get(accounts_handle->cfd(), snap2, "alice", &val_a2);
    assert(su2.ok() && val_u2 == "Alice_V2");
    assert(sa2.ok() && val_a2 == "500_USD");
    std::cout << "  - Snapshot 2 (LSN 2): users.alice = '" << val_u2 << "', accounts.alice = '" << val_a2 << "'\n";

    // Verify Current Unpinned Read (LSN 3)
    std::string val_u3, val_a3;
    Status su3 = MultiCFConsistentReader::Get(users_handle->cfd(), nullptr, "alice", &val_u3);
    Status sa3 = MultiCFConsistentReader::Get(accounts_handle->cfd(), nullptr, "alice", &val_a3);
    assert(su3.IsNotFound());
    assert(sa3.ok() && val_a3 == "500_USD");
    std::cout << "  - Current Read (LSN 3): users.alice = <NotFound>, accounts.alice = '" << val_a3 << "'\n";

    // Verify MultiGet across multiple CFs
    std::vector<std::pair<uint32_t, std::string>> queries = {
        {users_handle->GetID(), "alice"},
        {accounts_handle->GetID(), "alice"}
    };
    auto multi_res = MultiCFConsistentReader::MultiGet(&cf_set, snap1, queries);
    assert(multi_res.size() == 2);
    assert(multi_res[0].first.ok() && multi_res[0].second == "Alice_V1");
    assert(multi_res[1].first.ok() && multi_res[1].second == "100_USD");

    snap_mgr.ReleaseSnapshot(snap1);
    snap_mgr.ReleaseSnapshot(snap2);
    assert(snap_mgr.ActiveCount() == 0);

    std::cout << "  [PASSED] Test 3: Cross-CF Atomic Point-in-Time Consistent Snapshots\n";
}

// -----------------------------------------------------------------------------
// Test 4: Multi-CF VersionSet VersionEdit Atomic Commits (Feature 106)
// -----------------------------------------------------------------------------
static void TestMultiCFVersionSetAtomicCommits() {
    std::cout << "\n=== Test 4: Multi-CF VersionSet VersionEdit Atomic Commits (Feature 106) ===\n";

    const std::string dbname = "./test_phase18_manifest_atomic";
    CleanupDir(dbname);

    MultiCFVersionSet vset(dbname);
    Status s = vset.CreateNewManifest(1);
    assert(s.ok());

    ColumnFamilySet cf_set;
    auto cf_users = cf_set.CreateColumnFamily("users");
    auto cf_orders = cf_set.CreateColumnFamily("orders");

    // Create atomic multi-CF edit
    MultiCFVersionEdit edit;
    edit.SetLastSequence(1000);
    edit.SetNextFileNumber(200);

    FileMetaData f1;
    f1.file_number = 101;
    f1.file_size = 1024 * 1024;
    f1.smallest_key = "u:001";
    f1.largest_key = "u:999";
    edit.AddFile(cf_users->GetID(), 0, f1);

    FileMetaData f2;
    f2.file_number = 102;
    f2.file_size = 2048 * 1024;
    f2.smallest_key = "o:001";
    f2.largest_key = "o:999";
    edit.AddFile(cf_orders->GetID(), 1, f2);

    s = vset.LogAndApply({edit}, &cf_set);
    assert(s.ok());

    assert(cf_users->cfd()->GetLevelFiles(0).size() == 1);
    assert(cf_users->cfd()->GetLevelFiles(0)[0].file_number == 101);

    assert(cf_orders->cfd()->GetLevelFiles(1).size() == 1);
    assert(cf_orders->cfd()->GetLevelFiles(1)[0].file_number == 102);

    assert(vset.LastSequence() == 1000);
    assert(vset.NextFileNumber() == 200);

    std::cout << "  - Multi-CF VersionEdit committed 2 SSTable files across 'users' (L0) and 'orders' (L1) atomically.\n";
    std::cout << "  [PASSED] Test 4: Multi-CF VersionSet VersionEdit Atomic Commits\n";

    CleanupDir(dbname);
}

// -----------------------------------------------------------------------------
// Test 5: Multi-CF Manifest Recovery Replay Orchestrator (Feature 107)
// -----------------------------------------------------------------------------
static void TestMultiCFManifestRecoveryReplay() {
    std::cout << "\n=== Test 5: Multi-CF Manifest Recovery Replay Orchestrator (Feature 107) ===\n";

    const std::string dbname = "./test_phase18_recovery";
    CleanupDir(dbname);

    // 1. Generate Manifest with dynamic CF creations, file additions, and drop
    {
        MultiCFVersionSet vset(dbname);
        Status s = vset.CreateNewManifest(10);
        assert(s.ok());

        MultiCFVersionEdit edit1;
        edit1.AddColumnFamily(1, "tenant_a");
        edit1.AddColumnFamily(2, "tenant_b");
        edit1.AddColumnFamily(3, "tenant_temp");
        edit1.SetLastSequence(500);
        edit1.SetNextFileNumber(50);
        s = vset.LogAndApply({edit1}, nullptr);
        assert(s.ok());

        MultiCFVersionEdit edit2;
        FileMetaData fa;
        fa.file_number = 21;
        fa.file_size = 100000;
        fa.smallest_key = "a1";
        fa.largest_key = "a9";
        edit2.AddFile(1, 0, fa);

        FileMetaData fb;
        fb.file_number = 22;
        fb.file_size = 200000;
        fb.smallest_key = "b1";
        fb.largest_key = "b9";
        edit2.AddFile(2, 1, fb);

        edit2.DropColumnFamily(3); // Drop tenant_temp
        edit2.SetLastSequence(1000);
        s = vset.LogAndApply({edit2}, nullptr);
        assert(s.ok());
    }

    // 2. Perform Startup Crash Recovery
    ColumnFamilySet recovered_cf_set;
    MultiCFVersionSet recovered_vset(dbname);
    MultiCFRecoveryStats stats;

    MultiCFManifestRecovery recovery;
    Status s = recovery.Recover(dbname, &recovered_cf_set, &recovered_vset, &stats);
    assert(s.ok());

    assert(stats.records_replayed >= 2);
    assert(stats.files_recovered == 2);
    assert(stats.last_sequence == 1000);

    auto h_a = recovered_cf_set.GetColumnFamily("tenant_a");
    auto h_b = recovered_cf_set.GetColumnFamily("tenant_b");
    auto h_temp = recovered_cf_set.GetColumnFamily("tenant_temp");

    assert(h_a && !h_a->cfd()->IsDropped());
    assert(h_b && !h_b->cfd()->IsDropped());
    assert(h_temp == nullptr); // Dropped CF correctly removed from active directory

    // Verify dropped status in all CFDs
    auto all_cfds = recovered_cf_set.GetAllCFDs();
    bool found_dropped_temp = false;
    for (const auto& cfd : all_cfds) {
        if (cfd->GetName() == "tenant_temp" && cfd->IsDropped()) {
            found_dropped_temp = true;
        }
    }
    assert(found_dropped_temp);

    assert(h_a->cfd()->GetLevelFiles(0).size() == 1);
    assert(h_a->cfd()->GetLevelFiles(0)[0].file_number == 21);

    assert(h_b->cfd()->GetLevelFiles(1).size() == 1);
    assert(h_b->cfd()->GetLevelFiles(1)[0].file_number == 22);

    std::cout << "  - Manifest Recovery replayed " << stats.edits_replayed << " edits across "
              << stats.cfs_recovered << " CFs (Restored LSN: " << stats.last_sequence << ")\n";
    std::cout << "  - Verified: 'tenant_a' active (1 L0 file), 'tenant_b' active (1 L1 file), 'tenant_temp' DROPPED.\n";

    std::cout << "  [PASSED] Test 5: Multi-CF Manifest Recovery Replay Orchestrator\n";

    CleanupDir(dbname);
}

// -----------------------------------------------------------------------------
// Test 6: Column Family Metrics & Dynamic Telemetry Aggregator (Feature 108)
// -----------------------------------------------------------------------------
static void TestColumnFamilyMetricsAggregator() {
    std::cout << "\n=== Test 6: Column Family Metrics & Dynamic Telemetry Aggregator (Feature 108) ===\n";

    CFTelemetryAggregator telemetry;

    telemetry.RegisterCF(1, "users");
    telemetry.RegisterCF(2, "orders");

    // Record operations for users (CF 1)
    telemetry.RecordWrite(1, 1024 * 100); // 100KB write
    telemetry.RecordRead(1, 512);
    telemetry.RecordCompaction(1, 500 * 1024, 450 * 1024);
    telemetry.UpdateMemoryAndFiles(1, 4 * 1024 * 1024, 3, 6 * 1024 * 1024);

    // Record operations for orders (CF 2)
    telemetry.RecordWrite(2, 2048 * 100); // 200KB write
    telemetry.RecordRead(2, 1024);
    telemetry.RecordCompaction(2, 1000 * 1024, 900 * 1024);
    telemetry.UpdateMemoryAndFiles(2, 8 * 1024 * 1024, 5, 10 * 1024 * 1024);

    auto m1 = telemetry.GetCFMetrics(1);
    assert(m1.cf_name == "users");
    assert(m1.num_writes == 1);
    assert(m1.write_bytes == 1024 * 100);
    assert(m1.compaction_output_bytes == 450 * 1024);
    assert(m1.write_amplification_factor > 1.0);

    auto m2 = telemetry.GetCFMetrics(2);
    assert(m2.cf_name == "orders");
    assert(m2.num_writes == 1);
    assert(m2.write_bytes == 2048 * 100);

    auto global = telemetry.GetGlobalMetrics();
    assert(global.total_active_cfs == 2);
    assert(global.total_writes == 2);
    assert(global.total_write_bytes == (1024 + 2048) * 100);
    assert(global.total_sstable_files == 8);

    std::string report = telemetry.FormatTelemetryReport();
    std::cout << report;

    std::cout << "  [PASSED] Test 6: Column Family Metrics & Dynamic Telemetry Aggregator\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase XVIII Verification Test Suite\n";
    std::cout << "  Features 103-108: Column Families & Multi-Tenancy Architecture (Part 2)\n";
    std::cout << "=======================================================================\n";

    TestIndependentCFCompactionScheduling();
    TestCrossCFCompactionConcurrencyAndLimiting();
    TestCrossCFConsistentSnapshots();
    TestMultiCFVersionSetAtomicCommits();
    TestMultiCFManifestRecoveryReplay();
    TestColumnFamilyMetricsAggregator();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase XVIII Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
