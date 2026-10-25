#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <algorithm>
#include <thread>
#include <atomic>

#include "version_edit.hpp"
#include "version.hpp"
#include "version_set.hpp"
#include "current_file.hpp"
#include "manifest_recovery.hpp"
#include "manifest_compactor.hpp"
#include "wal_format.hpp"
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

static void CreateFakeSSTable(const std::string& dbname, uint64_t file_num) {
    char sst_name[64];
    snprintf(sst_name, sizeof(sst_name), "/%06llu.sst", static_cast<unsigned long long>(file_num));
    std::string path = dbname + sst_name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << "FAKESSTDATA";
    f.close();
}

// -----------------------------------------------------------------------------
// Test 1: VersionEdit Varint Roundtrip Encoding (Feature 92)
// -----------------------------------------------------------------------------
static void Test1_VersionEditVarintRoundtrip() {
    std::cout << "\n=== Test 1: VersionEdit Varint Roundtrip Encoding (Feature 92) ===\n";

    VersionEdit edit1;
    edit1.SetComparatorName(Slice("lsm.InternalKeyComparator"));
    edit1.SetLogNumber(1001);
    edit1.SetNextFile(2005);
    edit1.SetLastSequence(50000);

    edit1.SetCompactPointer(1, InternalKey("user_k1", 100, kTypeValue));
    edit1.SetCompactPointer(2, InternalKey("user_k2", 200, kTypeValue));

    for (int i = 0; i < 10; ++i) {
        int level = i % 3;
        uint64_t file_num = 100 + i;
        uint64_t file_size = 1024 * (i + 1);
        char k_start[32], k_end[32];
        snprintf(k_start, sizeof(k_start), "key_start_%03d", i * 10);
        snprintf(k_end, sizeof(k_end), "key_end_%03d", i * 10 + 9);
        edit1.AddFile(level, file_num, file_size,
                      InternalKey(k_start, 10, kTypeValue),
                      InternalKey(k_end, 20, kTypeValue));
    }

    for (int i = 0; i < 5; ++i) {
        edit1.DeleteFile(i % 3, 50 + i);
    }

    std::string encoded;
    edit1.EncodeTo(&encoded);
    assert(!encoded.empty());

    VersionEdit edit2;
    Status s = edit2.DecodeFrom(Slice(encoded));
    assert(s.ok());

    assert(edit2.has_comparator() && edit2.comparator() == "lsm.InternalKeyComparator");
    assert(edit2.has_log_number() && edit2.log_number() == 1001);
    assert(edit2.has_next_file_number() && edit2.next_file_number() == 2005);
    assert(edit2.has_last_sequence() && edit2.last_sequence() == 50000);

    assert(edit2.compact_pointers().size() == 2);
    assert(edit2.new_files().size() == 10);
    assert(edit2.deleted_files().size() == 5);

    // Verify individual added file properties
    for (size_t i = 0; i < edit2.new_files().size(); ++i) {
        assert(edit2.new_files()[i].first == edit1.new_files()[i].first);
        assert(edit2.new_files()[i].second.file_number == edit1.new_files()[i].second.file_number);
        assert(edit2.new_files()[i].second.file_size == edit1.new_files()[i].second.file_size);
        assert(edit2.new_files()[i].second.smallest_key == edit1.new_files()[i].second.smallest_key);
        assert(edit2.new_files()[i].second.largest_key == edit1.new_files()[i].second.largest_key);
    }

    std::cout << "  - Encoded VersionEdit size: " << encoded.size() << " bytes.\n";
    std::cout << "  - " << edit2.DebugString() << "\n";
    std::cout << "  [PASSED] Test 1: VersionEdit Varint Roundtrip Encoding\n";
}

// -----------------------------------------------------------------------------
// Test 2: Version Reference Counting & Lifetime (Feature 93)
// -----------------------------------------------------------------------------
static void Test2_VersionReferenceCounting() {
    std::cout << "\n=== Test 2: Version Reference Counting & Lifetime (Feature 93) ===\n";

    auto* v = new Version(nullptr);
    assert(v->RefCount() == 0);

    // Reader 1, 2, 3 acquire leases
    v->Ref();
    v->Ref();
    v->Ref();
    assert(v->RefCount() == 3);

    // Reader 1 and Reader 2 release
    v->Unref();
    v->Unref();
    assert(v->RefCount() == 1);

    // Reader 3 releases -> version deallocates itself cleanly
    v->Unref();

    std::cout << "  - Verified atomic ref-counting: 0 -> 3 -> 1 -> 0 (clean deallocation).\n";
    std::cout << "  [PASSED] Test 2: Version Reference Counting & Lifetime\n";
}

// -----------------------------------------------------------------------------
// Test 3: Atomic CURRENT File Transition (Feature 94)
// -----------------------------------------------------------------------------
static void Test3_AtomicCurrentFileTransition() {
    std::cout << "\n=== Test 3: Atomic CURRENT File Transition (Feature 94) ===\n";

    std::string test_dir = "./test_phase16_current";
    SetupDir(test_dir);

    // Initial switch to manifest 1
    Status s1 = SetCurrentFile(test_dir, 1);
    assert(s1.ok());

    std::string current_manifest;
    Status s2 = ReadCurrentFile(test_dir, &current_manifest);
    assert(s2.ok());
    assert(current_manifest == "MANIFEST-000001");

    // Concurrently write and read CURRENT to verify zero torn reads
    std::atomic<bool> stop_flag{false};
    std::thread reader_th([&]() {
        while (!stop_flag.load(std::memory_order_relaxed)) {
            std::string manifest_read;
            Status s = ReadCurrentFile(test_dir, &manifest_read);
            assert(s.ok());
            assert(manifest_read.rfind("MANIFEST-", 0) == 0);
            assert(manifest_read.size() == 15);
        }
    });

    for (uint64_t m = 2; m <= 100; ++m) {
        Status s = SetCurrentFile(test_dir, m);
        assert(s.ok());
    }

    stop_flag.store(true, std::memory_order_relaxed);
    reader_th.join();

    Status s_final = ReadCurrentFile(test_dir, &current_manifest);
    assert(s_final.ok());
    assert(current_manifest == "MANIFEST-000100");

    std::cout << "  - Verified 100 concurrent atomic CURRENT file updates with zero torn reads.\n";
    std::cout << "  - Final CURRENT pointer: " << current_manifest << "\n";
    std::cout << "  [PASSED] Test 3: Atomic CURRENT File Transition\n";
}

// -----------------------------------------------------------------------------
// Test 4: Manifest Roll-Forward Replay (Feature 95)
// -----------------------------------------------------------------------------
static void Test4_ManifestRollForwardReplay() {
    std::cout << "\n=== Test 4: Manifest Roll-Forward Replay (Feature 95) ===\n";

    std::string test_dir = "./test_phase16_replay";
    SetupDir(test_dir);

    {
        VersionSet vset(test_dir);

        // Apply 20 consecutive VersionEdit delta records
        for (int i = 1; i <= 20; ++i) {
            VersionEdit e;
            e.SetNextFile(100 + i);
            e.SetLastSequence(i * 50);
            e.SetLogNumber(i * 10);
            // Add file to level i % 4
            e.AddFile(i % 4, 100 + i, 1024 * i,
                      InternalKey("k_start", i, kTypeValue),
                      InternalKey("k_end", i, kTypeValue));
            if (i > 5) {
                // Delete file from earlier edit
                e.DeleteFile((i - 5) % 4, 100 + (i - 5));
            }
            Status s = vset.LogAndApply(&e);
            assert(s.ok());
        }

        assert(vset.current()->TotalFiles() == 5); // 5 files remaining (files 116..120)
        assert(vset.LastSequence() == 1000);
        assert(vset.NextFileNumber() == 120);
    }

    // Replay entire manifest log into fresh VersionSet instance
    VersionSet recovered_vset(test_dir);
    size_t edits_applied = 0;
    size_t files_verified = 0;
    Status s = ManifestRecovery::RecoverAndVerify(test_dir, &recovered_vset, &edits_applied, &files_verified);
    assert(s.ok());

    assert(edits_applied == 21); // 1 baseline snapshot edit + 20 delta edits
    assert(files_verified == 5);
    assert(recovered_vset.LastSequence() == 1000);
    assert(recovered_vset.NextFileNumber() == 120);
    assert(recovered_vset.current()->TotalFiles() == 5);

    std::cout << "  - Applied 20 delta edits; recovered exact state across 4 levels.\n";
    std::cout << "  - State: " << recovered_vset.Summary() << "\n";
    std::cout << "  [PASSED] Test 4: Manifest Roll-Forward Replay\n";
}

// -----------------------------------------------------------------------------
// Test 5: Missing File Integrity Rejection (Feature 95)
// -----------------------------------------------------------------------------
static void Test5_MissingFileIntegrityRejection() {
    std::cout << "\n=== Test 5: Missing File Integrity Rejection (Feature 95) ===\n";

    std::string test_dir = "./test_phase16_missing_file";
    SetupDir(test_dir);

    // Create a VersionSet with 2 SSTable files (101 and 102)
    {
        VersionSet vset(test_dir);
        VersionEdit e;
        e.SetNextFile(103);
        e.SetLastSequence(500);
        e.AddFile(0, 101, 1024, InternalKey("a", 1, kTypeValue), InternalKey("m", 1, kTypeValue));
        e.AddFile(0, 102, 1024, InternalKey("n", 1, kTypeValue), InternalKey("z", 1, kTypeValue));
        Status s = vset.LogAndApply(&e);
        assert(s.ok());

        // Create only SSTable 101 on disk, intentionally omitting SSTable 102
        CreateFakeSSTable(test_dir, 101);
    }

    // Attempt recovery with file verification enabled -> must detect missing 000102.sst
    VersionSet recovered_vset(test_dir);
    size_t edits_applied = 0;
    size_t files_verified = 0;
    Status s_rec = ManifestRecovery::RecoverAndVerify(
        test_dir,
        &recovered_vset,
        &edits_applied,
        &files_verified,
        /*verify_sstable_files=*/true
    );

    assert(!s_rec.ok());
    assert(s_rec.IsCorruption());
    std::cout << "  - Missing SSTable detected during recovery: " << s_rec.ToString() << "\n";
    std::cout << "  [PASSED] Test 5: Missing File Integrity Rejection\n";
}

// -----------------------------------------------------------------------------
// Test 6: Manifest Snapshot Compaction (Feature 96)
// -----------------------------------------------------------------------------
static void Test6_ManifestSnapshotCompaction() {
    std::cout << "\n=== Test 6: Manifest Snapshot Compaction (Feature 96) ===\n";

    std::string test_dir = "./test_phase16_compaction";
    SetupDir(test_dir);

    VersionSet vset(test_dir);

    // Append 100 small incremental delta edits
    for (int i = 1; i <= 100; ++i) {
        VersionEdit e;
        e.SetNextFile(200 + i);
        e.SetLastSequence(i * 10);
        e.AddFile(i % 5, 200 + i, 2048,
                  InternalKey("k_start", i, kTypeValue),
                  InternalKey("k_end", i, kTypeValue));
        Status s = vset.LogAndApply(&e);
        assert(s.ok());
    }

    assert(vset.current()->TotalFiles() == 100);

    // Trigger Manifest Snapshot Compaction
    uint64_t new_manifest_num = 0;
    std::string old_manifest_name;
    Status s_roll = ManifestCompactor::RollManifest(test_dir, &vset, &new_manifest_num, &old_manifest_name);
    assert(s_roll.ok());

    // Verify CURRENT pointer matches new manifest
    std::string current_manifest;
    ReadCurrentFile(test_dir, &current_manifest);
    char expected_manifest[32];
    snprintf(expected_manifest, sizeof(expected_manifest), "MANIFEST-%06llu", static_cast<unsigned long long>(new_manifest_num));
    assert(current_manifest == expected_manifest);

    // Verify old manifest was garbage-collected
    std::string old_path = test_dir + "/" + old_manifest_name;
    std::error_code ec;
    assert(!fs::exists(old_path, ec));

    // Verify fresh recovery from compacted manifest restores all 100 files in 1 edit
    VersionSet fresh_recovery(test_dir);
    size_t edits_applied = 0;
    size_t files_verified = 0;
    Status s_rec = ManifestRecovery::RecoverAndVerify(test_dir, &fresh_recovery, &edits_applied, &files_verified);
    assert(s_rec.ok());
    assert(edits_applied == 1); // Exactly 1 consolidated snapshot edit!
    assert(files_verified == 100);
    assert(fresh_recovery.current()->TotalFiles() == 100);

    std::cout << "  - Compacted 100 incremental edits into 1 consolidated snapshot edit.\n";
    std::cout << "  - CURRENT updated to " << current_manifest << "; old manifest cleaned up.\n";
    std::cout << "  - Fast Recovery: verified 100 active files in single edit scan.\n";
    std::cout << "  [PASSED] Test 6: Manifest Snapshot Compaction\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase XVI Verification Test Suite\n";
    std::cout << "  Features 91-96: Metadata, Manifest & Version Architecture (Part 1)\n";
    std::cout << "=======================================================================\n";

    Test1_VersionEditVarintRoundtrip();
    Test2_VersionReferenceCounting();
    Test3_AtomicCurrentFileTransition();
    Test4_ManifestRollForwardReplay();
    Test5_MissingFileIntegrityRejection();
    Test6_ManifestSnapshotCompaction();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase XVI Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
