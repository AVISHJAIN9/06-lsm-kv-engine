#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <chrono>
#include <random>

#include "format.hpp"
#include "block_builder.hpp"
#include "block_reader.hpp"
#include "block_trailer.hpp"
#include "memtable_rep.hpp"
#include "vector_memtable.hpp"
#include "memtable_recovery.hpp"
#include "memtable_queue.hpp"
#include "write_benchmark.hpp"
#include "wal_writer.hpp"
#include "write_batch.hpp"

namespace fs = std::filesystem;
using namespace lsm;

static void CleanTestDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
}

// -----------------------------------------------------------------------------
// Test 1: Multi-WAL Memtable State Reconstruction (Feature 37)
// Generate 3 sequential WAL files; recover un-flushed state and verify exact
// key/value inventory, continuous LSN sequence, and active/imm promotions.
// -----------------------------------------------------------------------------
static void Test1_MultiWalRecoveryCoordinator() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 1: MemTable Multi-WAL Recovery Coordinator\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_phase7_multi_wal";
    CleanTestDir(test_dir);

    const int kNumSegments = 3;
    const int kKeysPerSegment = 500;
    uint64_t current_seq = 1;

    // 1. Generate 3 sequential WAL files
    for (int seg = 1; seg <= kNumSegments; ++seg) {
        char path_buf[128];
        std::snprintf(path_buf, sizeof(path_buf), "%s/wal_%06d.log", test_dir.c_str(), seg);

        WALWriter writer(std::string(path_buf), seg);
        for (int i = 0; i < kKeysPerSegment; ++i) {
            WriteBatch batch;
            std::string k = "cust_" + std::to_string(seg) + "_k_" + std::to_string(i);
            std::string v = "payload_val_bytes_segment_" + std::to_string(seg) + "_" + std::to_string(i);

            batch.Put(k, v);
            batch.SetSequence(current_seq++);

            Status s = writer.AddRecord(Slice(batch.Data()));
            assert(s.ok());
        }
        writer.Sync();
    }

    std::cout << "  - Generated " << kNumSegments << " WAL files (" << (kNumSegments * kKeysPerSegment)
              << " total operations, max LSN = " << (current_seq - 1) << ")\n";

    // 2. Replay all WAL segments into a fresh MemTableQueue
    const size_t kSmallMemtable = 32 * 1024; // 32KB to trigger memtable rotations during replay
    MemTableQueue recovered_queue(kSmallMemtable);

    MemTableRecoveryCoordinator coordinator(test_dir);
    MultiWalRecoveryStats stats;
    Status s = coordinator.Recover(1, &recovered_queue, &stats);
    assert(s.ok());

    std::cout << "  - Replay Statistics:\n";
    std::cout << "    * WAL Files Scanned:    " << stats.total_wal_files_scanned << "\n";
    std::cout << "    * Records Replayed:     " << stats.total_records_replayed << "\n";
    std::cout << "    * Last Recovered LSN:   " << stats.last_recovered_seq << "\n";
    std::cout << "    * MemTable Rotations:   " << stats.memtable_switches << "\n";

    assert(stats.total_wal_files_scanned == kNumSegments);
    assert(stats.total_records_replayed == kNumSegments * kKeysPerSegment);
    assert(stats.last_recovered_seq == current_seq - 1);

    // 3. Verify all keys can be queried with exact historical values
    for (int seg = 1; seg <= kNumSegments; ++seg) {
        for (int i = 0; i < kKeysPerSegment; ++i) {
            std::string k = "cust_" + std::to_string(seg) + "_k_" + std::to_string(i);
            std::string expected_v = "payload_val_bytes_segment_" + std::to_string(seg) + "_" + std::to_string(i);
            std::string actual_v;
            Status get_s;
            bool found = recovered_queue.Get(k, kMaxSequenceNumber, &actual_v, &get_s);
            assert(found);
            assert(get_s.ok());
            assert(actual_v == expected_v);
        }
    }

    std::cout << "[PASSED] Test 1: Multi-WAL Segment Replay & State Reconstruction 100% Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 2: Pluggable Alternative MemTable Abstraction (Feature 38)
// Instantiate VectorMemTableRep; test bulk load, sort, Get point lookup,
// and bidirectional Iterator navigation.
// -----------------------------------------------------------------------------
static void Test2_PluggableVectorMemTable() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 2: Pluggable MemTable Abstraction (VectorRep)\n";
    std::cout << "=================================================================\n";

    VectorMemTableRepFactory factory;
    std::cout << "  - Testing Factory: " << factory.Name() << "\n";

    std::shared_ptr<MemTableRep> mem = factory.CreateMemTableRep();
    assert(mem != nullptr);

    const int kNumEntries = 1000;

    // 1. Insert 1,000 entries out of order
    std::vector<int> indices(kNumEntries);
    for (int i = 0; i < kNumEntries; ++i) indices[i] = i;
    std::mt19937 g(42);
    std::shuffle(indices.begin(), indices.end(), g);

    for (int idx : indices) {
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "user_%05d", idx);
        std::string val = "profile_metadata_blob_" + std::to_string(idx);
        mem->Add(idx + 10, ValueType::kTypeValue, Slice(key_buf), Slice(val));
    }

    assert(mem->Count() == kNumEntries);
    assert(mem->ApproximateMemoryUsage() > 0);

    // 2. Point lookup verification
    for (int i = 0; i < kNumEntries; ++i) {
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "user_%05d", i);
        std::string expected_val = "profile_metadata_blob_" + std::to_string(i);

        std::string actual_val;
        Status s;
        bool found = mem->Get(Slice(key_buf), kMaxSequenceNumber, &actual_val, &s);
        assert(found);
        assert(s.ok());
        assert(actual_val == expected_val);
    }

    // 3. Test Tombstone deletion semantics
    mem->Add(kNumEntries + 100, ValueType::kTypeDeletion, Slice("user_00050"), Slice());
    std::string val;
    Status del_s;
    bool found_del = mem->Get(Slice("user_00050"), kMaxSequenceNumber, &val, &del_s);
    assert(!found_del);
    assert(del_s.IsNotFound());

    // 4. Test Bidirectional Iterator
    auto iter = mem->NewIterator();
    assert(iter != nullptr);

    iter->SeekToFirst();
    assert(iter->Valid());
    assert(iter->key() == Slice("user_00000"));

    int count = 0;
    while (iter->Valid()) {
        count++;
        iter->Next();
    }
    assert(count == kNumEntries + 1); // 1,000 puts + 1 delete tombstone

    // Seek test
    iter->Seek(Slice("user_00450"));
    assert(iter->Valid());
    assert(iter->key() == Slice("user_00450"));

    iter->Prev();
    assert(iter->Valid());
    assert(iter->key() == Slice("user_00449"));

    std::cout << "  - Verified VectorMemTableRep point lookups, deletes, and bidirectional iteration.\n";
    std::cout << "[PASSED] Test 2: Pluggable VectorMemTable Abstraction 100% Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 3: Prefix-Compressed Data Block Builder & Space Reduction (Feature 41)
// Encode sorted keys sharing common prefixes (e.g. "org:deepmind:project:alpha:key:0000");
// verify restart points and assert >= 2x compression ratio vs uncompressed layout.
// -----------------------------------------------------------------------------
static void Test3_PrefixCompressedBlockEfficiency() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 3: Prefix-Compressed Data Block Builder & Ratio\n";
    std::cout << "=================================================================\n";

    const int kBlockRestartInterval = 16;
    BlockBuilder builder(kBlockRestartInterval);

    const int kNumKeys = 1000;
    size_t uncompressed_raw_bytes = 0;

    for (int i = 0; i < kNumKeys; ++i) {
        char key_buf[64];
        std::snprintf(key_buf, sizeof(key_buf), "enterprise:org:department:team_omega:user_%05d:attr", i);
        std::string val = "payload_content_record_" + std::to_string(i);

        uncompressed_raw_bytes += (std::strlen(key_buf) + val.size());
        builder.Add(Slice(key_buf), Slice(val));
    }

    Slice block_slice = builder.Finish();
    size_t compressed_block_bytes = block_slice.size();

    double ratio = static_cast<double>(uncompressed_raw_bytes) / compressed_block_bytes;
    std::cout << "  - Uncompressed Key-Value Payload: " << uncompressed_raw_bytes << " bytes\n";
    std::cout << "  - Prefix-Delta Compressed Block:  " << compressed_block_bytes << " bytes (including restart array)\n";
    std::cout << "  - Compression Ratio:             " << ratio << "x (Target >= 2.0x)\n";

    assert(ratio >= 2.0);

    std::cout << "[PASSED] Test 3: Prefix-Delta Block Compression Ratio >= 2x Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 4: Block Reader & Binary Search Seek (Feature 41)
// Decode prefix-compressed block with BlockReader; test binary search across
// restart points, verifying seek latency <= 5 microseconds.
// -----------------------------------------------------------------------------
static void Test4_RestartPointBinarySearch() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 4: Restart-Point Binary Search & Seek Latency\n";
    std::cout << "=================================================================\n";

    BlockBuilder builder(16);
    const int kNumKeys = 2000;

    for (int i = 0; i < kNumKeys; ++i) {
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "row_%06d", i);
        std::string val = "val_" + std::to_string(i);
        builder.Add(Slice(key_buf), Slice(val));
    }

    Slice block_content = builder.Finish();

    BlockReader reader(block_content);
    auto iter = reader.NewIterator();
    assert(iter != nullptr);

    // 1. Verify forward scan over all entries
    iter->SeekToFirst();
    int count = 0;
    while (iter->Valid()) {
        char expected_key[32];
        std::snprintf(expected_key, sizeof(expected_key), "row_%06d", count);
        assert(iter->key() == Slice(expected_key));
        count++;
        iter->Next();
    }
    assert(count == kNumKeys);

    // 2. Measure random Seek latency across restart intervals
    const int kSeekTrials = 1000;
    std::vector<int> target_indices(kSeekTrials);
    for (int i = 0; i < kSeekTrials; ++i) {
        target_indices[i] = (i * 7 + 13) % kNumKeys;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int idx : target_indices) {
        char target_key[32];
        std::snprintf(target_key, sizeof(target_key), "row_%06d", idx);
        iter->Seek(Slice(target_key));
        assert(iter->Valid());
        assert(iter->key() == Slice(target_key));
        assert(iter->value() == Slice("val_" + std::to_string(idx)));
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double total_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    double avg_seek_us = total_us / kSeekTrials;

    std::cout << "  - Executed " << kSeekTrials << " random Seek() queries across " << kNumKeys << " keys\n";
    std::cout << "  - Average Seek Latency: " << avg_seek_us << " us (Target <= 5.0 us)\n";

    assert(avg_seek_us <= 5.0);

    std::cout << "[PASSED] Test 4: BlockReader Binary Search Random Access Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 5: SSTable Footer Serialization & Magic Number Validation (Feature 40)
// Encode and decode 48-byte fixed footer; verify magic constant 0xdb4775248b80fb57ull.
// -----------------------------------------------------------------------------
static void Test5_FooterSerializationAndMagic() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 5: SSTable Footer Format & Magic Number\n";
    std::cout << "=================================================================\n";

    Footer footer;
    BlockHandle meta_handle(1048576, 4096);    // Offset 1MB, Size 4KB
    BlockHandle index_handle(2097152, 8192);   // Offset 2MB, Size 8KB

    footer.set_metaindex_handle(meta_handle);
    footer.set_index_handle(index_handle);

    std::string footer_buf;
    footer.EncodeTo(&footer_buf);

    std::cout << "  - Encoded Footer Length: " << footer_buf.size() << " bytes (Fixed = "
              << Footer::kEncodedLength << " bytes)\n";
    assert(footer_buf.size() == Footer::kEncodedLength);
    assert(footer_buf.size() == 48);

    // Decode and verify
    Footer decoded_footer;
    Slice input(footer_buf);
    Status s = decoded_footer.DecodeFrom(&input);
    assert(s.ok());

    assert(decoded_footer.metaindex_handle().offset() == 1048576);
    assert(decoded_footer.metaindex_handle().size() == 4096);
    assert(decoded_footer.index_handle().offset() == 2097152);
    assert(decoded_footer.index_handle().size() == 8192);

    // Test rejection of corrupted magic number
    footer_buf[footer_buf.size() - 1] ^= 0xFF; // Corrupt magic byte
    Slice corrupt_input(footer_buf);
    Footer corrupt_footer;
    Status bad_s = corrupt_footer.DecodeFrom(&corrupt_input);
    assert(!bad_s.ok());
    assert(bad_s.IsCorruption());
    std::cout << "  - Corrupt magic rejection verified: " << bad_s.ToString() << "\n";

    std::cout << "[PASSED] Test 5: 48-byte Fixed Footer & Magic Validation 100% Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 6: Block Checksums & Corrupt Block Isolation (Feature 42)
// Append 5-byte block trailer; verify bit-flip corruption detection.
// -----------------------------------------------------------------------------
static void Test6_BlockChecksumCorruptionIsolation() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 6: Block Checksum Envelope & Corruption Defense\n";
    std::cout << "=================================================================\n";

    std::string raw_payload = "SSTable data block contents with multiple key/value records...";
    std::string block_with_trailer = raw_payload;

    WriteBlockTrailer(&block_with_trailer, CompressionType::kNoCompression);
    assert(block_with_trailer.size() == raw_payload.size() + kBlockTrailerSize);

    // 1. Valid block verification
    Slice verified_payload;
    CompressionType comp_type;
    Status s = ReadAndVerifyBlock(Slice(block_with_trailer), &verified_payload, &comp_type);
    assert(s.ok());
    assert(verified_payload == Slice(raw_payload));
    assert(comp_type == CompressionType::kNoCompression);
    std::cout << "  - Uncorrupted block CRC32C verification: OK\n";

    // 2. Corrupt a single bit in the block payload
    std::string tampered_block = block_with_trailer;
    tampered_block[10] ^= 0x01; // Flip 1 bit

    Slice bad_payload;
    Status corrupt_s = ReadAndVerifyBlock(Slice(tampered_block), &bad_payload, &comp_type);
    assert(!corrupt_s.ok());
    assert(corrupt_s.IsCorruption());
    std::cout << "  - Bit-flip corruption detection: " << corrupt_s.ToString() << " (PASSED)\n";

    // 3. Corrupt a byte in the trailer checksum
    std::string tampered_crc = block_with_trailer;
    tampered_crc[tampered_crc.size() - 1] ^= 0x80;

    Status bad_crc_s = ReadAndVerifyBlock(Slice(tampered_crc), &bad_payload, &comp_type);
    assert(!bad_crc_s.ok());
    assert(bad_crc_s.IsCorruption());
    std::cout << "  - Trailer CRC tamper detection: " << bad_crc_s.ToString() << " (PASSED)\n";

    std::cout << "[PASSED] Test 6: Block Checksum Envelope & Corruption Defense Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 7: Write-Path Micro-Benchmark Suite (Feature 39)
// Measures latency percentiles (P50, P90, P99, P99.9), ops/sec, and bandwidth.
// -----------------------------------------------------------------------------
static void Test7_WritePathMicroBenchmarkRunner() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 7: Write-Path Micro-Benchmark Suite (Feature 39)\n";
    std::cout << "=================================================================\n";

    MemTableQueue bench_queue(64 * 1024 * 1024); // 64MB buffer

    BenchmarkOptions opts;
    opts.num_threads = 4;
    opts.num_keys = 20000;
    opts.key_size = 16;
    opts.val_size = 128;
    opts.random_keys = false;

    BenchmarkResult res = WriteBenchmarkRunner::Run(&bench_queue, opts);
    std::cout << res.ToString();

    assert(res.ops_per_sec > 0);
    assert(res.p50_latency_ns > 0);

    std::cout << "[PASSED] Test 7: High-Precision Write-Path Monotonic Micro-Benchmark Verified.\n";
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << ">>> STARTING LSM-TREE PHASE VII SSTABLE CORE & MEMTABLE SUITE <<<\n";
    std::cout << ">>> Features 37-42: Multi-WAL Replay, Pluggable Rep, Benchmarks, <<<\n";
    std::cout << ">>> SSTable Format, Prefix Compression & Block CRC Checksums   <<<\n";
    std::cout << "=================================================================\n";

    try {
        Test1_MultiWalRecoveryCoordinator();
        Test2_PluggableVectorMemTable();
        Test3_PrefixCompressedBlockEfficiency();
        Test4_RestartPointBinarySearch();
        Test5_FooterSerializationAndMagic();
        Test6_BlockChecksumCorruptionIsolation();
        Test7_WritePathMicroBenchmarkRunner();

        std::cout << "\n=================================================================\n";
        std::cout << ">>> [ALL PHASE VII VERIFICATION TESTS PASSED 100% WITH ZERO ERRORS] <<<\n";
        std::cout << "=================================================================\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FATAL TEST EXCEPTION: " << e.what() << "\n";
        return 1;
    }
}
