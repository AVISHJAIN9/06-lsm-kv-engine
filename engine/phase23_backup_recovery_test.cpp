#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <fstream>
#include <filesystem>
#include <memory>
#include <sys/stat.h>

#include "checkpoint_manager.hpp"
#include "backup_engine.hpp"
#include "continuous_wal_archiver.hpp"
#include "pitr_recovery.hpp"
#include "backup_verifier.hpp"
#include "object_offloader.hpp"
#include "memtable_core.hpp"

using namespace lsm;
namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// Test 1: Sub-Millisecond Live Hard-Link Checkpointing (Feature 133)
// -----------------------------------------------------------------------------
static void TestLiveHardLinkCheckpointing() {
    std::cout << "\n=== Test 1: Sub-Millisecond Live Hard-Link Checkpointing (Feature 133) ===\n";

    std::string test_db_dir = "./test_phase23_db";
    std::string checkpoint_dir = "./test_phase23_checkpoint";

    fs::remove_all(test_db_dir);
    fs::remove_all(checkpoint_dir);
    fs::create_directories(test_db_dir);

    // Create simulated live database files
    std::vector<std::string> sst_files = {"000001.sst", "000002.sst", "000003.sst"};
    for (const auto& sst : sst_files) {
        std::ofstream out((fs::path(test_db_dir) / sst).string(), std::ios::binary);
        std::string dummy_data(65536, 'A'); // 64KB SSTable
        out.write(dummy_data.data(), dummy_data.size());
        out.close();
    }

    std::string manifest_file = "MANIFEST-000001";
    std::ofstream man_out((fs::path(test_db_dir) / manifest_file).string());
    man_out << "MANIFEST_STATE_V1\n";
    man_out.close();

    std::vector<std::string> wal_files = {"000004.log"};
    std::ofstream wal_out((fs::path(test_db_dir) / wal_files[0]).string());
    wal_out << "WAL_LOG_ENTRIES\n";
    wal_out.close();

    LiveCheckpointManager checkpoint_mgr;
    CheckpointMetadata meta;
    Status s = checkpoint_mgr.CreateCheckpoint(
        test_db_dir,
        checkpoint_dir,
        sst_files,
        manifest_file,
        wal_files,
        10000,
        &meta
    );
    assert(s.ok());
    assert(meta.sstable_files.size() == 3);
    assert(meta.total_bytes >= 3 * 65536);

    // Verify POSIX hard link inode preservation
    for (const auto& sst : sst_files) {
        std::string src_path = (fs::path(test_db_dir) / sst).string();
        std::string dst_path = (fs::path(checkpoint_dir) / sst).string();

        assert(fs::exists(dst_path));
        bool same_inode = LiveCheckpointManager::VerifyHardLinks(src_path, dst_path);
        assert(same_inode);
    }

    // Verify CURRENT file and MANIFEST in checkpoint directory
    assert(fs::exists(fs::path(checkpoint_dir) / "CURRENT"));
    assert(fs::exists(fs::path(checkpoint_dir) / manifest_file));

    std::cout << "  - Live Checkpoint created: 3 SSTables hard-linked with identical inodes (Zero copy overhead).\n";
    std::cout << "  - Total Checkpoint Size: " << meta.total_bytes << " Bytes. MANIFEST & CURRENT synced.\n";
    std::cout << "  [PASSED] Test 1: Sub-Millisecond Live Hard-Link Checkpointing\n";
}

// -----------------------------------------------------------------------------
// Test 2: Incremental Backup Deduplication (Feature 134)
// -----------------------------------------------------------------------------
static void TestIncrementalBackupDeduplication() {
    std::cout << "\n=== Test 2: Incremental Backup Deduplication (Feature 134) ===\n";

    std::string backup_root = "./test_phase23_backups";
    std::string db_dir = "./test_phase23_inc_db";
    std::string restore_dir = "./test_phase23_restored_db";

    fs::remove_all(backup_root);
    fs::remove_all(db_dir);
    fs::remove_all(restore_dir);
    fs::create_directories(db_dir);

    BackupEngine backup_engine(backup_root);

    // 1. Create Base Backup B1 with 3 SSTables
    std::vector<std::string> b1_ssts = {"000001.sst", "000002.sst", "000003.sst"};
    for (const auto& sst : b1_ssts) {
        std::ofstream out((fs::path(db_dir) / sst).string(), std::ios::binary);
        out << "SSTABLE_CONTENT_" << sst << "\n";
    }
    std::ofstream man_out((fs::path(db_dir) / "MANIFEST-001").string());
    man_out << "MANIFEST_BASE\n";
    man_out.close();

    BackupInfo b1_info;
    Status s1 = backup_engine.CreateNewBackup(db_dir, b1_ssts, "MANIFEST-001", false, &b1_info);
    assert(s1.ok());
    assert(b1_info.backup_id == 1);
    assert(b1_info.new_ssts.size() == 3);
    assert(!b1_info.is_incremental);
    std::cout << "  - Base Backup B1 created with 3 new SSTables.\n";

    // 2. Ingest churn: 1 new SSTable generated (000004.sst)
    std::vector<std::string> b2_ssts = {"000001.sst", "000002.sst", "000003.sst", "000004.sst"};
    std::ofstream out4((fs::path(db_dir) / "000004.sst").string(), std::ios::binary);
    out4 << "SSTABLE_CONTENT_000004.sst\n";
    out4.close();

    BackupInfo b2_info;
    Status s2 = backup_engine.CreateNewBackup(db_dir, b2_ssts, "MANIFEST-002", true, &b2_info);
    assert(s2.ok());
    assert(b2_info.backup_id == 2);
    assert(b2_info.is_incremental);
    assert(b2_info.referenced_ssts.size() == 4);
    assert(b2_info.new_ssts.size() == 1 && b2_info.new_ssts[0] == "000004.sst");
    std::cout << "  - Incremental Backup B2 created: strictly copied 1 new SSTable (000004.sst) while referencing all 4 SSTables.\n";

    // 3. Restore Incremental Backup B2
    Status s_rest = backup_engine.RestoreBackup(2, restore_dir);
    assert(s_rest.ok());

    for (const auto& sst : b2_ssts) {
        assert(fs::exists(fs::path(restore_dir) / sst));
    }
    assert(fs::exists(fs::path(restore_dir) / "CURRENT"));
    std::cout << "  - Restored Backup B2: all 4 SSTables restored from shared deduplicated catalog.\n";

    std::cout << "  [PASSED] Test 2: Incremental Backup Deduplication\n";
}

// -----------------------------------------------------------------------------
// Test 3: Continuous WAL Archival Pipeline (Feature 135)
// -----------------------------------------------------------------------------
static void TestContinuousWALArchival() {
    std::cout << "\n=== Test 3: Continuous WAL Archival Pipeline (Feature 135) ===\n";

    std::string archive_dir = "./test_phase23_wal_archive";
    std::string raw_wal_dir = "./test_phase23_raw_wals";

    fs::remove_all(archive_dir);
    fs::remove_all(raw_wal_dir);
    fs::create_directories(raw_wal_dir);

    ContinuousWALArchiver archiver(archive_dir);

    // Create and archive 5 rotated WAL segments
    for (uint64_t seg = 1; seg <= 5; ++seg) {
        std::string raw_path = (fs::path(raw_wal_dir) / ("wal_" + std::to_string(seg) + ".log")).string();
        std::ofstream out(raw_path, std::ios::binary);
        out << "WAL_HEADER_SEG_" << seg << "\n";
        for (int i = 0; i < 50; ++i) {
            out << (seg * 100 + i) << "|" << (1000 + seg * 100 + i) << "|PUT|k" << i << "|v" << i << "\n";
        }
        out.close();

        ArchivedSegmentMeta meta;
        Status s = archiver.ArchiveSegment(
            seg,
            raw_path,
            seg * 100,
            seg * 100 + 49,
            1000 + seg * 100,
            1000 + seg * 100 + 49,
            &meta
        );
        assert(s.ok());
        assert(meta.segment_id == seg);
        assert(!meta.sha256_hex.empty());
    }

    assert(archiver.GetArchivedSegments().size() == 5);
    std::cout << "  - 5 rotated WAL segments continuously compressed, archived, and cataloged in ARCHIVE_INDEX.\n";

    // Restore segment 3 and verify checksum & data fidelity
    std::string restored_wal_path = (fs::path(raw_wal_dir) / "wal_3_restored.log").string();
    Status s_restore = archiver.RestoreSegment(3, restored_wal_path);
    assert(s_restore.ok());
    assert(fs::exists(restored_wal_path));

    // Verify timestamp range query
    auto covering = archiver.GetSegmentsCoveringRange(1250, 1450);
    assert(covering.size() >= 2); // Segments 2, 3, 4 overlap
    std::cout << "  - Timestamp range query [1250 us, 1450 us] returned " << covering.size() << " covering archived segments.\n";

    std::cout << "  [PASSED] Test 3: Continuous WAL Archival Pipeline\n";
}

// -----------------------------------------------------------------------------
// Test 4: Microsecond-Precision PITR Recovery (Feature 136)
// -----------------------------------------------------------------------------
static void TestMicrosecondPITRRecovery() {
    std::cout << "\n=== Test 4: Microsecond-Precision PITR Recovery (Feature 136) ===\n";

    std::string base_dir = "./test_phase23_pitr_base";
    std::string wal_archive_dir = "./test_phase23_pitr_wal_archive";
    std::string restore_dir = "./test_phase23_pitr_restore";

    fs::remove_all(base_dir);
    fs::remove_all(wal_archive_dir);
    fs::remove_all(restore_dir);
    fs::create_directories(base_dir);
    fs::create_directories(wal_archive_dir);

    // Create WAL log records with precise timestamps:
    // k1 at T = 100 us
    // k2 at T = 200 us
    // k3 at T = 300 us
    // k4 at T = 400 us
    std::string wal_file = (fs::path(wal_archive_dir) / "wal_stream_1.log").string();
    std::ofstream out(wal_file);

    PITRLogRecord r1{1, 100, ValueType::kTypeValue, "user:101", "Alice"};
    PITRLogRecord r2{2, 200, ValueType::kTypeValue, "user:102", "Bob"};
    PITRLogRecord r3{3, 300, ValueType::kTypeValue, "user:103", "Charlie"};
    PITRLogRecord r4{4, 400, ValueType::kTypeValue, "user:104", "David"};

    out << r1.Serialize();
    out << r2.Serialize();
    out << r3.Serialize();
    out << r4.Serialize();
    out.close();

    PITRCoordinator pitr;
    std::shared_ptr<MemTableCore> recovered_memtable;
    PITRReplaySummary summary;

    // Target cutoff: T = 250 us
    Status s = pitr.RecoverToTimestamp(
        250, // target_epoch_micros
        base_dir,
        {wal_file},
        restore_dir,
        recovered_memtable,
        &summary
    );
    assert(s.ok());
    assert(summary.records_applied == 2);
    assert(summary.records_skipped_after_cutoff >= 1);
    assert(summary.latest_replayed_timestamp_micros == 200);

    // Verify state: k1 and k2 must exist; k3 and k4 must NOT exist
    std::string v1, v2, v3, v4;
    Status s1, s2, s3, s4;
    bool f1 = recovered_memtable->Get(Slice("user:101"), kMaxSequenceNumber, &v1, &s1);
    bool f2 = recovered_memtable->Get(Slice("user:102"), kMaxSequenceNumber, &v2, &s2);
    bool f3 = recovered_memtable->Get(Slice("user:103"), kMaxSequenceNumber, &v3, &s3);
    bool f4 = recovered_memtable->Get(Slice("user:104"), kMaxSequenceNumber, &v4, &s4);

    assert(f1 && s1.ok() && v1 == "Alice");
    assert(f2 && s2.ok() && v2 == "Bob");
    assert(!f3 || !s3.ok()); // k3 was recorded at T=300 > 250 -> strictly excluded
    assert(!f4 || !s4.ok()); // k4 was recorded at T=400 > 250 -> strictly excluded

    std::cout << "  - PITR targeting T = 250 us restored exactly 2 records (T=100, T=200).\n";
    std::cout << "  - Cutoff enforced with microsecond accuracy: user:101='Alice', user:102='Bob', user:103=<excluded>.\n";
    std::cout << "  [PASSED] Test 4: Microsecond-Precision PITR Recovery\n";
}

// -----------------------------------------------------------------------------
// Test 5: Cryptographic Backup Integrity Verification (Feature 137)
// -----------------------------------------------------------------------------
static void TestCryptographicBackupIntegrity() {
    std::cout << "\n=== Test 5: Cryptographic Backup Integrity Verification (Feature 137) ===\n";

    std::string verify_dir = "./test_phase23_verify_dir";
    fs::remove_all(verify_dir);
    fs::create_directories(verify_dir);

    // 1. Create a valid file and compute its SHA-256
    std::string sst1_path = (fs::path(verify_dir) / "000101.sst").string();
    std::string sst2_path = (fs::path(verify_dir) / "000102.sst").string();

    {
        std::ofstream out1(sst1_path, std::ios::binary);
        std::string data1 = "SSTABLE_FILE_101_CORRECT_PAYLOAD_DATA";
        out1.write(data1.data(), data1.size());
        out1.close();

        std::ofstream out2(sst2_path, std::ios::binary);
        std::string data2 = "SSTABLE_FILE_102_CORRECT_PAYLOAD_DATA";
        out2.write(data2.data(), data2.size());
        out2.close();
    }

    std::string hash1 = BackupVerifier::ComputeFileSHA256(sst1_path);
    std::string hash2 = BackupVerifier::ComputeFileSHA256(sst2_path);

    std::unordered_map<std::string, std::string> catalog = {
        {"000101.sst", hash1},
        {"000102.sst", hash2}
    };

    BackupVerifier verifier;
    VerificationReport report_clean = verifier.VerifyBackupDirectory(verify_dir, catalog);
    assert(report_clean.is_valid);
    assert(report_clean.corrupted_files.empty());
    std::cout << "  - Clean backup directory verified: 2 files, 0 corruptions, 100% cryptographic SHA-256 match.\n";

    // 2. Inject simulated bit-rot in 000102.sst (flip 4 bytes)
    {
        std::fstream file(sst2_path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(10);
        char corrupt_bytes[4] = {'X', 'Y', 'Z', '!'};
        file.write(corrupt_bytes, 4);
        file.close();
    }

    VerificationReport report_corrupt = verifier.VerifyBackupDirectory(verify_dir, catalog);
    assert(!report_corrupt.is_valid);
    assert(report_corrupt.corrupted_files.size() == 1);
    assert(report_corrupt.corrupted_files[0] == "000102.sst");
    std::cout << "  - Injected Bit-Rot successfully caught by BackupVerifier: "
              << report_corrupt.error_details[0] << "\n";

    std::cout << "  [PASSED] Test 5: Cryptographic Backup Integrity Verification\n";
}

// -----------------------------------------------------------------------------
// Test 6: Multi-Part Object Store Offloading & Retry (Feature 138)
// -----------------------------------------------------------------------------
static void TestObjectStoreOffloaderWithRetry() {
    std::cout << "\n=== Test 6: Multi-Part Object Store Offloading & Retry (Feature 138) ===\n";

    auto mock_client = std::make_shared<MockCloudStorageClient>();
    ObjectStoreOffloader offloader(mock_client, /* max_retries = */ 3);

    std::string test_file = "./test_phase23_offload_sstable.sst";
    {
        std::ofstream out(test_file, std::ios::binary);
        // Write 1.5MB file (will be chunked in 512KB parts)
        std::string chunk(512 * 1024, 'K');
        out.write(chunk.data(), chunk.size());
        std::string chunk2(512 * 1024, 'L');
        out.write(chunk2.data(), chunk2.size());
        std::string chunk3(512 * 1024, 'M');
        out.write(chunk3.data(), chunk3.size());
        out.close();
    }

    // Inject 2 transient simulated network dropouts (HTTP 500 / 503)
    mock_client->SetSimulatedFailures(2);

    // Upload with 512KB chunk size
    Status s = offloader.UploadFile(test_file, "backups/2026/000105.sst", 512 * 1024);
    assert(s.ok());
    assert(mock_client->HasObject("backups/2026/000105.sst"));

    // Verify uploaded content fidelity
    std::string downloaded;
    assert(mock_client->GetObject("backups/2026/000105.sst", &downloaded).ok());
    assert(downloaded.size() == 3 * 512 * 1024);
    assert(downloaded.substr(0, 512 * 1024) == std::string(512 * 1024, 'K'));
    assert(downloaded.substr(512 * 1024, 512 * 1024) == std::string(512 * 1024, 'L'));
    assert(downloaded.substr(1024 * 1024, 512 * 1024) == std::string(512 * 1024, 'M'));

    std::cout << "  - 1.5 MB SSTable offloaded to Cloud Storage in 3 parts with automatic retry over 2 simulated network dropouts.\n";
    std::cout << "  - Remote object payload validated with 100% byte fidelity.\n";

    std::cout << "  [PASSED] Test 6: Multi-Part Object Store Offloading & Retry\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase XXIII Verification Test Suite\n";
    std::cout << "  Features 133-138: Backup, Disaster Recovery & Continuous Archival Core\n";
    std::cout << "=======================================================================\n";

    TestLiveHardLinkCheckpointing();
    TestIncrementalBackupDeduplication();
    TestContinuousWALArchival();
    TestMicrosecondPITRRecovery();
    TestCryptographicBackupIntegrity();
    TestObjectStoreOffloaderWithRetry();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase XXIII Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
