#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <thread>
#include <filesystem>
#include <fstream>
#include <random>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>

#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "write_batch.hpp"
#include "memtable.hpp"
#include "metrics.hpp"
#include "wal_format.hpp"
#include "wal_writer.hpp"
#include "wal_reader.hpp"
#include "wal_manager.hpp"
#include "wal_committer.hpp"
#include "group_commit.hpp"
#include "checkpoint.hpp"
#include "wal_archival.hpp"
#include "wal_crypto.hpp"
#include "recovery.hpp"
#include "db.hpp"

namespace fs = std::filesystem;
using namespace lsm;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion FAILED: " << (msg) \
                      << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while (0)

void CleanDirectory(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
}

// =========================================================================
// Test 1: Lock-Free Adaptive Group Commit Under Stress
// =========================================================================
void Test1_LockFreeGroupCommitStress() {
    std::cout << ">>> [TEST 1] Lock-Free Adaptive Group Commit Under Stress..." << std::endl;
    const std::string test_dir = "./test_phase4_lf_gc";
    CleanDirectory(test_dir);
    const std::string log_path = test_dir + "/lockfree_gc.log";

    int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "Failed to open log file");
    auto writable = std::make_unique<PosixWritableFile>(log_path, fd);
    WALWriter wal_writer(std::move(writable), 1);

    MemTable mem;
    std::atomic<SequenceNumber> last_seq{0};

    auto prepare_cb = [&last_seq](WriteBatch* batch) {
        SequenceNumber first_seq = last_seq.load(std::memory_order_relaxed) + 1;
        batch->SetSequence(first_seq);
        last_seq.store(first_seq + batch->Count() - 1, std::memory_order_relaxed);
    };

    auto apply_cb = [&mem](WriteBatch* batch) -> Status {
        class BatchApplier : public WriteBatch::Handler {
        public:
            BatchApplier(SequenceNumber seq, MemTable* m) : current_seq(seq), mem_ptr(m) {}
            void Put(const Slice& key, const Slice& value) override {
                mem_ptr->Add(current_seq++, ValueType::kTypeValue, key, value);
            }
            void Delete(const Slice& key) override {
                mem_ptr->Add(current_seq++, ValueType::kTypeDeletion, key, Slice());
            }
            SequenceNumber current_seq;
            MemTable* mem_ptr;
        } applier(batch->Sequence(), &mem);

        return batch->Iterate(&applier);
    };

    LockFreeGroupCommitter committer(&wal_writer, prepare_cb, apply_cb);

    constexpr int kThreads = 16;
    constexpr int kBatchesPerThread = 50;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&committer, t, &success_count]() {
            for (int b = 0; b < kBatchesPerThread; ++b) {
                WriteBatch batch;
                std::string k = "k_th" + std::to_string(t) + "_b" + std::to_string(b);
                std::string v = "v_data_" + std::to_string(t * 1000 + b);
                batch.Put(Slice(k), Slice(v));

                Status s = committer.Submit(&batch, (b % 5 == 0));
                if (s.ok()) {
                    success_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    TEST_ASSERT(success_count.load() == kThreads * kBatchesPerThread, "All batches must commit");
    std::cout << "    Committed " << success_count.load() << " batches across 16 threads in "
              << elapsed_ms << " ms. Total Batches Processed: " << committer.GetTotalBatchesProcessed()
              << ", Group Syncs: " << committer.GetTotalGroupSyncs() << std::endl;

    // Verify all keys in MemTable
    for (int t = 0; t < kThreads; ++t) {
        for (int b = 0; b < kBatchesPerThread; ++b) {
            std::string k = "k_th" + std::to_string(t) + "_b" + std::to_string(b);
            std::string expected_v = "v_data_" + std::to_string(t * 1000 + b);
            std::string actual_v;
            Status get_st;
            bool found = mem.Get(Slice(k), kMaxSequenceNumber, &actual_v, &get_st);
            TEST_ASSERT(found && get_st.ok(), "Key must exist in memtable");
            TEST_ASSERT(actual_v == expected_v, "Value must match committed payload");
        }
    }

    wal_writer.Close();
    std::cout << "[PASS] Test 1: Lock-Free Adaptive Group Commit verified." << std::endl;
}

// =========================================================================
// Test 2: WAL Crash Recovery & LSN Replay Engine
// =========================================================================
void Test2_WALCrashRecoveryAndLSNReplay() {
    std::cout << ">>> [TEST 2] WAL Crash Recovery & LSN Replay Engine..." << std::endl;
    const std::string db_dir = "./test_phase4_crash_recovery";
    CleanDirectory(db_dir);

    Options options;
    options.enable_wal = true;
    options.wal_dir = db_dir + "/wal";
    options.max_wal_file_size = 50 * 1024; // 50KB to trigger rotations

    // 1. First DB lifecycle: Write 500 keys across multiple rotated WALs
    {
        DB* db = nullptr;
        Status s = DB::Open(options, db_dir, &db);
        TEST_ASSERT(s.ok() && db != nullptr, "DB Open failed");

        WriteOptions write_opts;
        for (int i = 1; i <= 500; ++i) {
            std::string k = "user_rec_" + std::to_string(i);
            std::string v = "profile_payload_data_item_" + std::to_string(i * 17);
            s = db->Put(write_opts, Slice(k), Slice(v));
            TEST_ASSERT(s.ok(), "Put failed");
        }

        TEST_ASSERT(db->GetLatestSequenceNumber() == 500, "Sequence number should be 500");
        std::cout << "    Initial DB wrote 500 records, active sequence: "
                  << db->GetLatestSequenceNumber() << std::endl;

        // Simulate abrupt process crash / power outage (delete without flushing memtable to SSTable)
        delete db;
    }

    // 2. Second DB lifecycle: Open new DB instance from same directory -> executes recovery
    {
        DB* recovered_db = nullptr;
        Status s = DB::Open(options, db_dir, &recovered_db);
        TEST_ASSERT(s.ok() && recovered_db != nullptr, "DB Re-open failed");

        RecoveryReport report = recovered_db->GetRecoveryReport();
        std::cout << "    " << report.ToString() << std::endl;

        TEST_ASSERT(report.status.ok(), "Recovery status should be OK");
        TEST_ASSERT(report.total_records_replayed == 500, "Should have replayed exactly 500 records");
        TEST_ASSERT(recovered_db->GetLatestSequenceNumber() == 500, "Recovered sequence number must be 500");

        // Verify all 500 keys are restored in active MemTable
        ReadOptions read_opts;
        for (int i = 1; i <= 500; ++i) {
            std::string k = "user_rec_" + std::to_string(i);
            std::string expected_v = "profile_payload_data_item_" + std::to_string(i * 17);
            std::string actual_v;
            Status get_st = recovered_db->Get(read_opts, Slice(k), &actual_v);
            TEST_ASSERT(get_st.ok(), "Recovered key get failed");
            TEST_ASSERT(actual_v == expected_v, "Recovered value mismatch");
        }

        // Test writing a new key starts from sequence 501
        WriteOptions write_opts;
        s = recovered_db->Put(write_opts, Slice("user_rec_501"), Slice("new_record_post_recovery"));
        TEST_ASSERT(s.ok(), "Post-recovery put failed");
        TEST_ASSERT(recovered_db->GetLatestSequenceNumber() == 501, "Sequence must advance monotonically to 501");

        delete recovered_db;
    }

    std::cout << "[PASS] Test 2: WAL Crash Recovery & LSN Replay Engine verified." << std::endl;
}

// =========================================================================
// Test 3: Corrupt Tail Truncation & Fault Recovery (Self-Healing)
// =========================================================================
void Test3_CorruptTailTruncation() {
    std::cout << ">>> [TEST 3] Corrupt Tail Truncation & Self-Healing..." << std::endl;
    const std::string test_dir = "./test_phase4_torn_tail";
    CleanDirectory(test_dir);
    const std::string log_path = test_dir + "/000001.log";

    int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "Failed to open log file");
    auto writable = std::make_unique<PosixWritableFile>(log_path, fd);
    WALWriter writer(std::move(writable), 1);

    // Write 100 valid WriteBatches
    for (int i = 1; i <= 100; ++i) {
        WriteBatch batch;
        batch.SetSequence(i);
        batch.Put(Slice("valid_key_" + std::to_string(i)), Slice("valid_value_" + std::to_string(i)));
        Status s = writer.AddRecord(Slice(batch.Data()));
        TEST_ASSERT(s.ok(), "AddRecord failed");
    }
    writer.Close();

    uintmax_t valid_size = fs::file_size(log_path);

    // Inject torn write: append 13 bytes of garbage/partial header at the end of the log
    int corrupt_fd = open(log_path.c_str(), O_WRONLY | O_APPEND);
    TEST_ASSERT(corrupt_fd >= 0, "Failed to open log for corrupt tail append");
    const char garbage[13] = {0x12, 0x34, 0x56, 0x78, 0x05, 0x00, 0x01, 'T', 'O', 'R', 'N', '!', '!'};
    write(corrupt_fd, garbage, sizeof(garbage));
    close(corrupt_fd);

    uintmax_t torn_size = fs::file_size(log_path);
    TEST_ASSERT(torn_size == valid_size + 13, "Torn file size must include garbage tail");

    // Recover with RecoveryEngine
    RecoveryOptions rec_opts;
    rec_opts.truncate_corrupt_tail = true;
    rec_opts.verify_checksums = true;

    RecoveryEngine recovery(rec_opts);
    MemTable mem;
    SequenceNumber recovered_seq = 0;
    RecoveryReport report;

    Status s = recovery.Recover(test_dir, &mem, &recovered_seq, &report);
    TEST_ASSERT(s.ok(), "Recovery should succeed with tail truncation");
    TEST_ASSERT(report.torn_write_truncated == true, "Report must flag torn write truncated");
    TEST_ASSERT(report.total_records_replayed == 100, "Must recover all 100 valid records");
    TEST_ASSERT(recovered_seq == 100, "Sequence must be 100");

    // Verify file size on disk was truncated back to valid_size
    uintmax_t post_trunc_size = fs::file_size(log_path);
    TEST_ASSERT(post_trunc_size == valid_size, "File on disk must be truncated to valid size");
    std::cout << "    Torn write cleanly truncated from " << torn_size << " B to " << post_trunc_size << " B" << std::endl;

    // Verify all 100 keys are intact in MemTable
    for (int i = 1; i <= 100; ++i) {
        std::string k = "valid_key_" + std::to_string(i);
        std::string expected_v = "valid_value_" + std::to_string(i);
        std::string actual_v;
        Status st;
        bool found = mem.Get(Slice(k), kMaxSequenceNumber, &actual_v, &st);
        TEST_ASSERT(found && st.ok() && actual_v == expected_v, "Valid key mismatch");
    }

    std::cout << "[PASS] Test 3: Corrupt Tail Truncation & Self-Healing verified." << std::endl;
}

// =========================================================================
// Test 4: Checkpoint-Driven Safe WAL Truncation
// =========================================================================
void Test4_CheckpointDrivenWALTruncation() {
    std::cout << ">>> [TEST 4] Checkpoint-Driven Safe WAL Truncation..." << std::endl;
    const std::string test_dir = "./test_phase4_checkpoint";
    CleanDirectory(test_dir);

    CheckpointManager checkpoint_mgr;

    // Register 4 WAL segments
    std::string path1 = test_dir + "/000001.log";
    std::string path2 = test_dir + "/000002.log";
    std::string path3 = test_dir + "/000003.log";
    std::string path4 = test_dir + "/000004.log";

    // Create dummy files on disk
    std::ofstream(path1) << "WAL1_CONTENT";
    std::ofstream(path2) << "WAL2_CONTENT";
    std::ofstream(path3) << "WAL3_CONTENT";
    std::ofstream(path4) << "WAL4_CONTENT";

    checkpoint_mgr.RegisterWAL(1, path1, 1);
    checkpoint_mgr.UpdateWALMaxLSN(1, 100, 1024);
    checkpoint_mgr.MarkWALImmutable(1);

    checkpoint_mgr.RegisterWAL(2, path2, 101);
    checkpoint_mgr.UpdateWALMaxLSN(2, 200, 1024);
    checkpoint_mgr.MarkWALImmutable(2);

    checkpoint_mgr.RegisterWAL(3, path3, 201);
    checkpoint_mgr.UpdateWALMaxLSN(3, 300, 1024);
    checkpoint_mgr.MarkWALImmutable(3);

    // Active WAL 4 (not immutable, max_lsn 400)
    checkpoint_mgr.RegisterWAL(4, path4, 301);
    checkpoint_mgr.UpdateWALMaxLSN(4, 400, 512);

    // Scenario 1: MemTable flushes LSN 200. Eligible for truncation: WAL 1 and WAL 2
    auto eligible = checkpoint_mgr.GetEligibleWALsForTruncation(200);
    TEST_ASSERT(eligible.size() == 2, "Expected 2 eligible WALs for truncation at LSN 200");
    TEST_ASSERT(eligible[0].log_number == 1 && eligible[1].log_number == 2, "Eligible logs must be 1 and 2");

    std::vector<std::string> deleted;
    Status s = checkpoint_mgr.ExecuteTruncation(200, &deleted);
    TEST_ASSERT(s.ok(), "Truncation execution failed");
    TEST_ASSERT(deleted.size() == 2, "Should delete 2 files");

    TEST_ASSERT(!fs::exists(path1), "WAL 1 must be deleted on disk");
    TEST_ASSERT(!fs::exists(path2), "WAL 2 must be deleted on disk");
    TEST_ASSERT(fs::exists(path3), "WAL 3 must remain intact");
    TEST_ASSERT(fs::exists(path4), "WAL 4 (active) must remain intact");

    // Scenario 2: Active WAL cannot be truncated even if sequence satisfies
    eligible = checkpoint_mgr.GetEligibleWALsForTruncation(450);
    TEST_ASSERT(eligible.size() == 1, "Only immutable WAL 3 should be eligible, not active WAL 4");
    TEST_ASSERT(eligible[0].log_number == 3, "Only WAL 3 eligible");

    std::cout << "[PASS] Test 4: Checkpoint-Driven Safe WAL Truncation verified." << std::endl;
}

// =========================================================================
// Test 5: Immutable Cold-Tier Archival & Compression
// =========================================================================
void Test5_ImmutableColdTierArchival() {
    std::cout << ">>> [TEST 5] Immutable Cold-Tier Archival & Compression..." << std::endl;
    const std::string test_dir = "./test_phase4_cold_tier";
    CleanDirectory(test_dir);
    const std::string wal_path = test_dir + "/000005.log";
    const std::string archive_dir = test_dir + "/archive";

    // Create a 128KB WAL segment with repeating log record patterns
    std::string sample_data;
    for (int i = 0; i < 1024; ++i) {
        sample_data += "RECORD_HEADER_LSN_" + std::to_string(i) + "____________________";
    }
    std::ofstream(wal_path, std::ios::binary) << sample_data;

    ColdTierArchiver archiver(archive_dir);
    ArchiveManifest manifest;
    Status s = archiver.ArchiveSegment(5, wal_path, 1000, 2024, &manifest);
    TEST_ASSERT(s.ok(), "ArchiveSegment failed");

    std::cout << "    Archived WAL 5:\n"
              << "    - Original Size:   " << manifest.original_bytes << " B\n"
              << "    - Compressed Size: " << manifest.compressed_bytes << " B\n"
              << "    - Compression Ratio: "
              << (100.0 * manifest.compressed_bytes / manifest.original_bytes) << " %\n"
              << "    - SHA/CRC32C:      " << manifest.crc32c << std::endl;

    TEST_ASSERT(manifest.compressed_bytes < manifest.original_bytes, "Compressed size must be smaller");
    TEST_ASSERT(fs::exists(manifest.archive_file), "Archive file must exist on disk");

    // Restore from cold tier
    const std::string restored_path = test_dir + "/000005_restored.log";
    s = archiver.RestoreSegment(manifest.archive_file, restored_path, manifest.crc32c);
    TEST_ASSERT(s.ok(), "RestoreSegment failed");

    // Verify bit-for-bit identity
    std::ifstream ifs(restored_path, std::ios::binary);
    std::string restored_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    TEST_ASSERT(restored_content == sample_data, "Restored WAL must be bit-for-bit identical");

    std::cout << "[PASS] Test 5: Immutable Cold-Tier Archival & Compression verified." << std::endl;
}

// =========================================================================
// Test 6: Authenticated Payload Encryption (AEAD ChaCha20-Poly1305)
// =========================================================================
void Test6_AuthenticatedPayloadEncryption() {
    std::cout << ">>> [TEST 6] Authenticated Payload Encryption (AEAD ChaCha20-Poly1305)..." << std::endl;

    const std::string secret_key_str = "MasterSecretKey_ProductionPass2026";
    uint8_t key[crypto::kKeySize];
    crypto::ChaCha20Poly1305::GenerateKeyFromSecret(secret_key_str, key);

    const std::string plaintext = "CONFIDENTIAL_TRANSACTION_PAYLOAD_ACCOUNT_TRANSFER_$5000000";
    std::string encrypted_envelope;

    Status s = crypto::ChaCha20Poly1305::EncryptPayload(key, Slice(plaintext), &encrypted_envelope);
    TEST_ASSERT(s.ok(), "EncryptPayload failed");
    TEST_ASSERT(encrypted_envelope.size() == crypto::kCryptoHeaderSize + plaintext.size(), "Envelope size mismatch");
    TEST_ASSERT(encrypted_envelope.find(plaintext) == std::string::npos, "Plaintext must not leak in ciphertext");

    // 1. Valid Decryption
    std::string decrypted_plaintext;
    s = crypto::ChaCha20Poly1305::DecryptPayload(key, Slice(encrypted_envelope), &decrypted_plaintext);
    TEST_ASSERT(s.ok(), "DecryptPayload failed with valid key");
    TEST_ASSERT(decrypted_plaintext == plaintext, "Decrypted text mismatch");

    // 2. Tampered Ciphertext -> Immediate Authentication Failure
    std::string tampered_envelope = encrypted_envelope;
    tampered_envelope[crypto::kCryptoHeaderSize + 5] ^= 0x01; // Flip 1 bit in ciphertext payload
    std::string failed_output;
    s = crypto::ChaCha20Poly1305::DecryptPayload(key, Slice(tampered_envelope), &failed_output);
    TEST_ASSERT(!s.ok() && s.IsCorruption(), "Tampered ciphertext must fail AEAD tag validation");
    std::cout << "    Correctly rejected tampered ciphertext: " << s.ToString() << std::endl;

    // 3. Wrong Key -> Immediate Authentication Failure
    uint8_t wrong_key[crypto::kKeySize];
    crypto::ChaCha20Poly1305::GenerateKeyFromSecret("WRONG_SECRET_PASSWORD", wrong_key);
    s = crypto::ChaCha20Poly1305::DecryptPayload(wrong_key, Slice(encrypted_envelope), &failed_output);
    TEST_ASSERT(!s.ok() && s.IsCorruption(), "Wrong key must fail AEAD tag validation");
    std::cout << "    Correctly rejected decryption with incorrect key: " << s.ToString() << std::endl;

    std::cout << "[PASS] Test 6: Authenticated Payload Encryption (AEAD) verified." << std::endl;
}

// =========================================================================
// Main Test Runner
// =========================================================================
int main() {
    std::cout << "================================================================================" << std::endl;
    std::cout << "  LSM-Tree Key-Value Engine: Phase IV Verification Test Suite" << std::endl;
    std::cout << "  Features: 19 (Lock-Free Group Commit), 20 (LSN Crash Recovery)," << std::endl;
    std::cout << "            21 (Torn Tail Truncation),   22 (Checkpoint WAL Truncation)," << std::endl;
    std::cout << "            23 (Cold-Tier Archival),     24 (AEAD Payload Encryption)" << std::endl;
    std::cout << "================================================================================" << std::endl;

    Test1_LockFreeGroupCommitStress();
    Test2_WALCrashRecoveryAndLSNReplay();
    Test3_CorruptTailTruncation();
    Test4_CheckpointDrivenWALTruncation();
    Test5_ImmutableColdTierArchival();
    Test6_AuthenticatedPayloadEncryption();

    std::cout << "================================================================================" << std::endl;
    std::cout << "  ALL 6 PHASE IV VERIFICATION TESTS PASSED (100% SUCCESS, ZERO DEFECTS)!" << std::endl;
    std::cout << "================================================================================" << std::endl;

    return 0;
}
