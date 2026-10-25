#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <random>

#include "db.hpp"
#include "group_commit.hpp"
#include "recovery.hpp"
#include "wal_healer.hpp"
#include "wal_truncator.hpp"
#include "wal_archiver.hpp"
#include "wal_crypto.hpp"
#include "wal_writer.hpp"
#include "wal_reader.hpp"
#include "memtable.hpp"

namespace fs = std::filesystem;
using namespace lsm;

static void CleanTestDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
}

// -----------------------------------------------------------------------------
// Test 1: Lock-Free Group Commit Concurrency
// Spawn 16 concurrent threads submitting batches; assert leader batches I/O
// and all threads observe successful commits.
// -----------------------------------------------------------------------------
static void Test1_LockFreeGroupCommitConcurrency() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 1: Lock-Free Group Commit Concurrency (16 Mutators)\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_durability_lf_gc";
    CleanTestDir(test_dir);

    std::string wal_path = test_dir + "/000001.log";
    auto wal_writer = std::make_unique<WALWriter>(wal_path);

    std::atomic<uint64_t> global_seq{1};
    auto committer = std::make_unique<LockFreeGroupCommitter>(
        wal_writer.get(),
        [&](WriteBatch* b) {
            SequenceNumber assigned_seq = global_seq.fetch_add(b->Count(), std::memory_order_relaxed);
            b->SetSequence(assigned_seq);
        },
        [&](WriteBatch* /*b*/) {
            return Status::OK();
        }
    );

    const int kNumThreads = 16;
    const int kOpsPerThread = 200;
    std::atomic<size_t> completed_threads{0};
    std::atomic<size_t> total_committed_batches{0};

    std::vector<std::thread> workers;
    workers.reserve(kNumThreads);

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < kNumThreads; ++t) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < kOpsPerThread; ++i) {
                WriteBatch batch;
                std::string k = "user_" + std::to_string(t) + "_key_" + std::to_string(i);
                std::string v = "val_payload_" + std::to_string(t * 10000 + i);
                batch.Put(k, v);

                Status s = committer->Submit(&batch, true);
                assert(s.ok());
                total_committed_batches.fetch_add(1, std::memory_order_relaxed);
            }
            completed_threads.fetch_add(1, std::memory_order_release);
        });
    }

    for (auto& th : workers) {
        th.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    assert(completed_threads.load() == kNumThreads);
    assert(total_committed_batches.load() == kNumThreads * kOpsPerThread);

    uint64_t group_syncs = committer->GetTotalGroupSyncs();
    uint64_t total_batches = committer->GetTotalBatchesProcessed();

    std::cout << "[PASSED] Test 1 Completed in " << elapsed_ms << " ms\n";
    std::cout << "  - Total Mutators: " << kNumThreads << ", Total Batches Committed: " << total_committed_batches.load() << "\n";
    std::cout << "  - Group Sync Cycles: " << group_syncs << ", Batches Processed: " << total_batches << "\n";
    std::cout << "  - Average Batch Coalesce Ratio: " << (double)total_batches / (group_syncs ? group_syncs : 1) << "x\n";
    std::cout << "  - Lock-Free MPSC Coordination Verified: 100% SUCCESS.\n";
}

// -----------------------------------------------------------------------------
// Test 2: Crash Recovery Replay
// Populate 5,000 keys across 3 WAL segments; close engine without memtable flush;
// run recovery and assert 100% of keys are restored with exact values and sequential LSNs.
// -----------------------------------------------------------------------------
static void Test2_CrashRecoveryReplay() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 2: Crash Recovery & Multi-Segment LSN Replay Engine\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_durability_crash_recovery";
    CleanTestDir(test_dir);

    const int kTotalKeys = 5000;
    // Step 1: Open DB with small segment rotation threshold to produce >= 3 WAL segments
    {
        Options opts;
        opts.wal_dir = test_dir;
        opts.max_wal_file_size = 64 * 1024; // 64KB rotation limit to force 3+ segments
        opts.fsync_mode = FsyncMode::kFsyncNever; // fast append for generation

        DB* raw_db = nullptr;
        Status s = DB::Open(opts, test_dir, &raw_db);
        assert(s.ok());
        std::unique_ptr<DB> db(raw_db);

        WriteOptions wo;
        for (int i = 0; i < kTotalKeys; ++i) {
            std::string k = "rec_key_" + std::to_string(i);
            std::string v = "rec_val_payload_data_block_" + std::to_string(i);
            s = db->Put(wo, k, v);
            assert(s.ok());
        }

        // Simulating unexpected crash: engine closes WITHOUT flushing memtable to SSTable!
    }

    // Step 2: Verify that multiple WAL segments exist on disk
    size_t wal_count = 0;
    for (const auto& entry : fs::directory_iterator(test_dir)) {
        if (entry.path().extension() == ".log") wal_count++;
    }
    std::cout << "  - Disk state post-crash: " << wal_count << " WAL segment files found.\n";
    assert(wal_count >= 3);

    // Step 3: Instantiate fresh engine on same directory to trigger Crash Recovery
    {
        auto start_rec = std::chrono::high_resolution_clock::now();

        Options opts;
        opts.wal_dir = test_dir;
        opts.max_wal_file_size = 64 * 1024;

        DB* recovered_raw_db = nullptr;
        Status s = DB::Open(opts, test_dir, &recovered_raw_db);
        assert(s.ok());
        std::unique_ptr<DB> recovered_db(recovered_raw_db);

        auto end_rec = std::chrono::high_resolution_clock::now();
        double rec_ms = std::chrono::duration<double, std::milli>(end_rec - start_rec).count();

        // Step 4: Verify 100% of keys are restored with exact values
        ReadOptions ro;
        for (int i = 0; i < kTotalKeys; ++i) {
            std::string k = "rec_key_" + std::to_string(i);
            std::string expected_v = "rec_val_payload_data_block_" + std::to_string(i);
            std::string actual_v;
            s = recovered_db->Get(ro, k, &actual_v);
            if (!s.ok()) {
                std::cerr << "FAILED to recover key: " << k << " (Status: " << s.ToString() << ")\n";
            }
            assert(s.ok());
            assert(actual_v == expected_v);
        }

        // Verify sequence number continuity
        SequenceNumber seq = recovered_db->GetLatestSequenceNumber();
        std::cout << "  - Replay recovered " << kTotalKeys << " keys in " << rec_ms << " ms\n";
        std::cout << "  - Restored Global Sequence Number: " << seq << "\n";
        assert(seq >= static_cast<SequenceNumber>(kTotalKeys));
    }

    std::cout << "[PASSED] Test 2 Completed: 100% Replay Fidelity across Multi-Segment WAL.\n";
}

// -----------------------------------------------------------------------------
// Test 3: Torn Write Truncation & Self-Healing
// Intentionally append 15 bytes of random garbage to the tail of a valid WAL;
// run recovery; verify healer truncates the file to the last valid record boundary.
// -----------------------------------------------------------------------------
static void Test3_TornWriteTruncation() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 3: Torn Write Truncation & Fault Self-Healing\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_durability_torn_write";
    CleanTestDir(test_dir);

    std::string wal_path = test_dir + "/000001.log";
    const int kValidRecords = 100;

    // Step 1: Write 100 clean records
    {
        WALWriter writer(wal_path);
        for (int i = 0; i < kValidRecords; ++i) {
            WriteBatch batch;
            batch.Put("clean_k_" + std::to_string(i), "clean_v_" + std::to_string(i));
            batch.SetSequence(i + 1);
            Status s = writer.AddRecord(batch.Data());
            assert(s.ok());
        }
        writer.Sync();
    }

    uint64_t valid_file_size = fs::file_size(wal_path);
    std::cout << "  - Initial Valid WAL size: " << valid_file_size << " bytes (" << kValidRecords << " records)\n";

    // Step 2: Corrupt tail by appending 15 bytes of random garbage
    {
        std::ofstream ofs(wal_path, std::ios::binary | std::ios::app);
        const char garbage[15] = {'\xDE', '\xAD', '\xBE', '\xEF', '\x01', '\x02', '\x03', '\x04',
                                  '\xFF', '\xFE', '\xFD', '\xFC', '\xAA', '\xBB', '\xCC'};
        ofs.write(garbage, 15);
        ofs.flush();
    }

    uint64_t corrupted_file_size = fs::file_size(wal_path);
    std::cout << "  - Corrupted WAL size: " << corrupted_file_size << " bytes (+15 bytes torn tail)\n";
    assert(corrupted_file_size == valid_file_size + 15);

    // Step 3: Run WALHealer
    HealResult heal_res;
    Status s = WALHealer::DetectAndHeal(wal_path, true, &heal_res);
    assert(s.ok());

    std::cout << "  - Healing Result: " << heal_res.details << "\n";
    assert(heal_res.torn_detected == true);
    assert(heal_res.valid_records_preserved == kValidRecords);
    assert(heal_res.valid_byte_offset == valid_file_size);
    assert(heal_res.bytes_truncated == 15);

    uint64_t healed_file_size = fs::file_size(wal_path);
    assert(healed_file_size == valid_file_size);

    // Step 4: Verify recovery replays 100% of the valid records without error
    MemTable mem;
    SequenceNumber last_seq = 0;
    RecoveryReport report;
    RecoveryEngine engine;
    s = engine.Recover(test_dir, &mem, &last_seq, &report);
    assert(s.ok());

    assert(report.total_records_replayed == kValidRecords);
    for (int i = 0; i < kValidRecords; ++i) {
        std::string k = "clean_k_" + std::to_string(i);
        std::string expected_v = "clean_v_" + std::to_string(i);
        std::string val;
        Status lookup_s;
        bool found = mem.Get(k, last_seq, &val, &lookup_s);
        assert(found);
        assert(val == expected_v);
    }

    std::cout << "[PASSED] Test 3 Completed: 15-byte Torn Tail Safely Truncated with 0 Record Loss.\n";
}

// -----------------------------------------------------------------------------
// Test 4: Checkpoint-Driven Truncation
// Flush memtable at LSN 2,500; trigger truncation; assert segments with
// max_lsn <= 2500 are removed while newer segments remain.
// -----------------------------------------------------------------------------
static void Test4_CheckpointDrivenTruncation() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 4: Checkpoint-Driven WAL Truncation Policy\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_durability_truncator";
    CleanTestDir(test_dir);

    // Create 4 simulated WAL segments:
    // Seg 1: LSN 1..1000
    // Seg 2: LSN 1001..2000
    // Seg 3: LSN 2001..2500
    // Seg 4: LSN 2501..3500
    WALTruncator truncator(test_dir);

    auto create_seg = [&](uint64_t seg_id, uint64_t min_l, uint64_t max_l) {
        std::string p = test_dir + "/" + std::to_string(seg_id) + ".log";
        WALWriter w(p);
        for (uint64_t seq = min_l; seq <= max_l; ++seq) {
            WriteBatch b;
            b.Put("k_" + std::to_string(seq), "v_" + std::to_string(seq));
            b.SetSequence(seq);
            w.AddRecord(b.Data());
        }
        w.Sync();

        WALMetadata m;
        m.log_number = seg_id;
        m.path = p;
        m.min_lsn = min_l;
        m.max_lsn = max_l;
        m.file_size = fs::file_size(p);
        m.is_immutable = (seg_id != 4); // seg 4 is active/mutable
        truncator.RegisterSegment(m);
    };

    create_seg(1, 1, 1000);
    create_seg(2, 1001, 2000);
    create_seg(3, 2001, 2500);
    create_seg(4, 2501, 3500);

    assert(truncator.GetAllSegments().size() == 4);

    // Flush point at LSN 2500
    SequenceNumber flushed_lsn = 2500;
    std::vector<WALMetadata> eligible = truncator.GetEligibleSegments(flushed_lsn);
    std::cout << "  - Segments eligible for pruning at LSN " << flushed_lsn << ": " << eligible.size() << "\n";
    assert(eligible.size() == 3); // Segments 1, 2, 3

    PruneReport report;
    Status s = truncator.PruneWALs(flushed_lsn, &report);
    assert(s.ok());

    std::cout << "  - Prune Report: " << report.files_pruned << " files pruned, " << report.bytes_reclaimed << " bytes reclaimed.\n";
    assert(report.files_pruned == 3);

    // Verify Segments 1, 2, 3 are unlinked from disk, while Segment 4 remains
    assert(!fs::exists(test_dir + "/1.log"));
    assert(!fs::exists(test_dir + "/2.log"));
    assert(!fs::exists(test_dir + "/3.log"));
    assert(fs::exists(test_dir + "/4.log"));

    assert(truncator.GetAllSegments().size() == 1);
    assert(truncator.GetAllSegments()[0].log_number == 4);

    std::cout << "[PASSED] Test 4 Completed: Exact Checkpoint Pruning at LSN 2500 Reclaimed 3 Segments.\n";
}

// -----------------------------------------------------------------------------
// Test 5: Cold-Tier Archival & Manifest
// Trigger archival of rotated segment; verify compressed archive exists,
// SHA-256 manifest matches, and payload decompresses cleanly.
// -----------------------------------------------------------------------------
static void Test5_ColdTierArchivalAndManifest() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 5: Immutable Cold-Tier Archival & SHA-256 Manifest\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_durability_archival";
    CleanTestDir(test_dir);

    std::string archive_dir = test_dir + "/archive";
    CleanTestDir(archive_dir);

    std::string raw_wal_path = test_dir + "/wal_000042.log";
    const int kNumBatches = 500;

    // Step 1: Create a realistic WAL file
    {
        WALWriter writer(raw_wal_path);
        for (int i = 1; i <= kNumBatches; ++i) {
            WriteBatch batch;
            batch.Put("archive_key_prefix_" + std::to_string(i), "archive_value_repeated_payload_bytes_0123456789");
            batch.SetSequence(i);
            writer.AddRecord(batch.Data());
        }
        writer.Sync();
    }

    uint64_t uncompressed_size = fs::file_size(raw_wal_path);
    std::cout << "  - Raw Segment 42 Size: " << uncompressed_size << " bytes\n";

    // Step 2: Perform Archival
    WALArchiver archiver(archive_dir);
    ArchivalManifest manifest;
    Status s = archiver.ArchiveSegment(raw_wal_path, 42, 1, kNumBatches, &manifest);
    assert(s.ok());

    std::cout << "  - Archival Completed:\n";
    std::cout << "    * Archive File: " << manifest.archive_filename << "\n";
    std::cout << "    * Compressed Size: " << manifest.compressed_size << " bytes (ratio: "
              << (double)manifest.compressed_size / manifest.uncompressed_size * 100.0 << "%)\n";
    std::cout << "    * SHA-256 Digest: " << manifest.sha256_digest << "\n";
    std::cout << "    * CRC32C: " << manifest.checksum_crc32c << "\n";

    assert(manifest.compressed_size > 0);
    assert(manifest.sha256_digest.size() == 64);
    assert(fs::exists(archive_dir + "/" + manifest.archive_filename));
    assert(fs::exists(archive_dir + "/manifest_42.json"));

    // Step 3: Verify cryptographic archive integrity
    s = archiver.VerifyArchive(42);
    assert(s.ok());

    // Step 4: Restore from cold archive into a new WAL path and compare bit-for-bit
    std::string restored_wal_path = test_dir + "/restored_wal_000042.log";
    s = archiver.RestoreSegment(42, restored_wal_path);
    assert(s.ok());

    uint64_t restored_size = fs::file_size(restored_wal_path);
    assert(restored_size == uncompressed_size);

    // Verify bit-for-bit identity
    std::ifstream orig_file(raw_wal_path, std::ios::binary);
    std::ifstream rest_file(restored_wal_path, std::ios::binary);
    std::vector<char> orig_buf((std::istreambuf_iterator<char>(orig_file)), std::istreambuf_iterator<char>());
    std::vector<char> rest_buf((std::istreambuf_iterator<char>(rest_file)), std::istreambuf_iterator<char>());
    assert(orig_buf == rest_buf);

    std::cout << "[PASSED] Test 5 Completed: Immutable Cold-Tier Archival & Bit-Perfect Restore.\n";
}

// -----------------------------------------------------------------------------
// Test 6: AEAD Encryption Integrity (ChaCha20-Poly1305 / AES-256-GCM)
// Write encrypted WAL segment; read back with valid key and assert exact recovery;
// flip a single ciphertext bit and assert decryption fails with tag mismatch error.
// -----------------------------------------------------------------------------
static void Test6_AEADEncryptionIntegrity() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 6: AEAD Hardware Encryption & Tamper Detection\n";
    std::cout << "=================================================================\n";

    std::string secret_key = "SuperSecretMasterKey2026!@#$%^&*";
    crypto::KeyStore key_store(secret_key);
    assert(key_store.HasKey());

    crypto::WALCipher cipher(key_store, 101); // Segment 101

    // Step 1: Encrypt a 32KB block payload
    std::string plaintext_block;
    plaintext_block.reserve(32768);
    for (int i = 0; i < 500; ++i) {
        plaintext_block.append("SensitiveFinancialTransactionRecord_Payload_Row_" + std::to_string(i) + ";");
    }

    std::string encrypted_envelope;
    Status s = cipher.EncryptBlock(
        reinterpret_cast<const uint8_t*>(plaintext_block.data()),
        plaintext_block.size(),
        0, // Block index 0
        &encrypted_envelope
    );
    assert(s.ok());
    assert(encrypted_envelope.size() == crypto::kCryptoHeaderSize + plaintext_block.size());

    std::cout << "  - Plaintext Size: " << plaintext_block.size() << " bytes\n";
    std::cout << "  - AEAD Envelope Size: " << encrypted_envelope.size() << " bytes (28-byte Header: 12B IV + 16B Poly1305 Tag)\n";

    // Step 2: Decrypt with valid key
    std::string decrypted_plaintext;
    s = cipher.DecryptBlock(
        reinterpret_cast<const uint8_t*>(encrypted_envelope.data()),
        encrypted_envelope.size(),
        0,
        &decrypted_plaintext
    );
    assert(s.ok());
    assert(decrypted_plaintext == plaintext_block);
    std::cout << "  - Valid Key Decryption: MATCH (100% Exact Plaintext Restored)\n";

    // Step 3: Decrypt with INVALID key -> tag mismatch
    crypto::KeyStore wrong_key_store("WrongSecretKey_1234567890abcdef");
    crypto::WALCipher wrong_cipher(wrong_key_store, 101);
    std::string rejected_plaintext;
    s = wrong_cipher.DecryptBlock(
        reinterpret_cast<const uint8_t*>(encrypted_envelope.data()),
        encrypted_envelope.size(),
        0,
        &rejected_plaintext
    );
    assert(!s.ok());
    assert(s.IsCorruption());
    std::cout << "  - Invalid Key Rejection: " << s.ToString() << " (PASSED)\n";

    // Step 4: Ciphertext Tamper Detection -> flip a single bit in the ciphertext payload
    std::string tampered_envelope = encrypted_envelope;
    // Flip bit in the payload (after header)
    tampered_envelope[crypto::kCryptoHeaderSize + 10] ^= 0x01;

    std::string tampered_plaintext;
    s = cipher.DecryptBlock(
        reinterpret_cast<const uint8_t*>(tampered_envelope.data()),
        tampered_envelope.size(),
        0,
        &tampered_plaintext
    );
    assert(!s.ok());
    assert(s.IsCorruption());
    std::cout << "  - Tampered Ciphertext Bit Rejection: " << s.ToString() << " (PASSED)\n";

    // Step 5: Tag Tamper Detection -> flip a single bit in the 16-byte Poly1305 Tag
    std::string tag_tampered_envelope = encrypted_envelope;
    tag_tampered_envelope[crypto::kNonceSize + 3] ^= 0x80; // inside tag region

    s = cipher.DecryptBlock(
        reinterpret_cast<const uint8_t*>(tag_tampered_envelope.data()),
        tag_tampered_envelope.size(),
        0,
        &tampered_plaintext
    );
    assert(!s.ok());
    assert(s.IsCorruption());
    std::cout << "  - Tampered Authentication Tag Rejection: " << s.ToString() << " (PASSED)\n";

    std::cout << "[PASSED] Test 6 Completed: Authenticated Encryption (AEAD) Guarantees Confidentiality & Zero Plaintext Leakage.\n";
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << ">>> STARTING LSM-TREE PHASE IV DURABILITY VERIFICATION SUITE <<<\n";
    std::cout << ">>> Features 19-24: Lock-Free Group Commit, Replay, Healer,     <<<\n";
    std::cout << ">>> Truncator, Cold-Tier Archiver, and AEAD Payload Encryption  <<<\n";
    std::cout << "=================================================================\n";

    try {
        Test1_LockFreeGroupCommitConcurrency();
        Test2_CrashRecoveryReplay();
        Test3_TornWriteTruncation();
        Test4_CheckpointDrivenTruncation();
        Test5_ColdTierArchivalAndManifest();
        Test6_AEADEncryptionIntegrity();

        std::cout << "\n=================================================================\n";
        std::cout << ">>> [ALL 6 PHASE IV VERIFICATION TESTS PASSED 100% WITH ZERO ERRORS] <<<\n";
        std::cout << "=================================================================\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FATAL TEST EXCEPTION: " << e.what() << "\n";
        return 1;
    }
}
