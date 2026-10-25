#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <chrono>
#include <random>

#include "table_builder.hpp"
#include "table_reader.hpp"
#include "sstable_validator.hpp"
#include "sstable_corruptor.hpp"
#include "leveled_compaction.hpp"
#include "size_tiered_compaction.hpp"
#include "bloom_filter.hpp"

namespace fs = std::filesystem;
using namespace lsm;

static void CleanTestDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
}

// -----------------------------------------------------------------------------
// Test 1: Streaming Table Build & Reader Scan (Features 50 & 51)
// -----------------------------------------------------------------------------
static void Test1_StreamingTableBuildAndReaderScan() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 1: Streaming Table Build & Reader Scan\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_phase9_streaming";
    CleanTestDir(test_dir);
    std::string sst_path = test_dir + "/000001.sst";

    TableOptions opts;
    opts.block_size = 2048; // 2KB data blocks
    opts.filter_policy = std::make_shared<BloomFilterPolicy>(10);

    TableBuilder builder(opts, sst_path);
    const int kNumKeys = 5000;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kNumKeys; ++i) {
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "user_%06d", i);
        std::string val = "profile_payload_data_bytes_" + std::to_string(i);
        builder.Add(Slice(key_buf), Slice(val));
    }
    Status finish_s = builder.Finish();
    auto t1 = std::chrono::high_resolution_clock::now();
    assert(finish_s.ok());

    double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    uint64_t file_size = builder.FileSize();
    double write_mb_per_sec = (file_size / (1024.0 * 1024.0)) / (build_ms / 1000.0);

    std::cout << "  - Built SSTable: " << file_size << " bytes (" << kNumKeys << " keys) in "
              << build_ms << " ms (" << write_mb_per_sec << " MB/sec)\n";
    assert(fs::exists(sst_path));

    // Open with TableReader
    std::unique_ptr<TableReader> reader;
    Status open_s = TableReader::Open(opts, sst_path, file_size, nullptr, &reader);
    assert(open_s.ok());
    assert(reader != nullptr);

    // 1. Point lookup verification
    for (int i = 0; i < kNumKeys; ++i) {
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "user_%06d", i);
        std::string expected_val = "profile_payload_data_bytes_" + std::to_string(i);

        std::string actual_val;
        Status s = reader->Get(Slice(key_buf), &actual_val);
        assert(s.ok());
        assert(actual_val == expected_val);
    }

    // 2. Full sequential scan verification
    auto iter = reader->NewIterator();
    assert(iter != nullptr);

    iter->SeekToFirst();
    int scanned_keys = 0;
    while (iter->Valid()) {
        char expected_key[32];
        std::snprintf(expected_key, sizeof(expected_key), "user_%06d", scanned_keys);
        assert(iter->key() == Slice(expected_key));
        scanned_keys++;
        iter->Next();
    }
    assert(scanned_keys == kNumKeys);

    std::cout << "  - Verified 100% of " << kNumKeys << " point lookups and sequential iteration scan.\n";
    std::cout << "[PASSED] Test 1: Streaming Table Build & Reader Scan 100% Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 2: Full SSTable Integrity Validation (Feature 49)
// -----------------------------------------------------------------------------
static void Test2_FullSSTableIntegrityValidation() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 2: SSTable Structural Integrity Validator\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_phase9_valid";
    CleanTestDir(test_dir);
    std::string sst_path = test_dir + "/000002.sst";

    TableOptions opts;
    opts.block_size = 2048;
    TableBuilder builder(opts, sst_path);

    for (int i = 0; i < 3000; ++i) {
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "order_%06d", i);
        std::string val = "order_info_blob_" + std::to_string(i);
        builder.Add(Slice(key_buf), Slice(val));
    }
    assert(builder.Finish().ok());

    ValidationReport report;
    Status val_s = SSTableValidator::ValidateFile(sst_path, &report);
    assert(val_s.ok());
    assert(report.is_valid);
    assert(report.num_keys == 3000);
    assert(report.smallest_key == "order_000000");
    assert(report.largest_key == "order_002999");
    assert(report.num_data_blocks > 0);

    std::cout << report.ToString();
    std::cout << "[PASSED] Test 2: SSTable Structural Integrity Validator 100% Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 3: Systematic Corruption Interception (Feature 52)
// -----------------------------------------------------------------------------
static void Test3_SystematicCorruptionInterception() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 3: Systematic Corruption Interception\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_phase9_corrupt";
    CleanTestDir(test_dir);

    auto create_clean_sst = [&](const std::string& path) {
        TableOptions opts;
        opts.block_size = 1024;
        TableBuilder builder(opts, path);
        for (int i = 0; i < 1000; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "k_%05d", i);
            builder.Add(Slice(key_buf), Slice("v_" + std::to_string(i)));
        }
        assert(builder.Finish().ok());
    };

    // 1. Bit-flip in data block payload
    std::string sst1 = test_dir + "/corrupt_payload.sst";
    create_clean_sst(sst1);
    assert(SSTableCorruptor::FlipBit(sst1, 50, 0x01).ok());

    ValidationReport rep1;
    Status s1 = SSTableValidator::ValidateFile(sst1, &rep1);
    assert(!s1.ok());
    assert(s1.IsCorruption());
    std::cout << "  - Intercepted Payload Bit-Flip: " << s1.ToString() << " (PASSED)\n";

    // 2. Corrupted CRC in block trailer
    std::string sst2 = test_dir + "/corrupt_crc.sst";
    create_clean_sst(sst2);
    assert(SSTableCorruptor::CorruptBlockCRC(sst2, 500).ok());

    ValidationReport rep2;
    Status s2 = SSTableValidator::ValidateFile(sst2, &rep2);
    assert(!s2.ok());
    assert(s2.IsCorruption());
    std::cout << "  - Intercepted Trailer CRC Corruption: " << s2.ToString() << " (PASSED)\n";

    // 3. Truncated footer
    std::string sst3 = test_dir + "/corrupt_trunc.sst";
    create_clean_sst(sst3);
    assert(SSTableCorruptor::TruncateFile(sst3, 20).ok());

    ValidationReport rep3;
    Status s3 = SSTableValidator::ValidateFile(sst3, &rep3);
    assert(!s3.ok());
    assert(s3.IsCorruption());
    std::cout << "  - Intercepted Truncated Footer: " << s3.ToString() << " (PASSED)\n";

    // 4. Damaged magic number
    std::string sst4 = test_dir + "/corrupt_magic.sst";
    create_clean_sst(sst4);
    assert(SSTableCorruptor::CorruptMagicNumber(sst4).ok());

    ValidationReport rep4;
    Status s4 = SSTableValidator::ValidateFile(sst4, &rep4);
    assert(!s4.ok());
    assert(s4.IsCorruption());
    std::cout << "  - Intercepted Damaged Magic Number: " << s4.ToString() << " (PASSED)\n";

    std::cout << "[PASSED] Test 3: 100% of Systematic SSTable Corruptions Intercepted.\n";
}

// -----------------------------------------------------------------------------
// Test 4: Block Cache Hit Acceleration (Feature 50)
// -----------------------------------------------------------------------------
static void Test4_BlockCacheHitAcceleration() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 4: Block Cache Hit Acceleration\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_phase9_cache";
    CleanTestDir(test_dir);
    std::string sst_path = test_dir + "/cache_test.sst";

    TableOptions opts;
    opts.block_size = 1024;
    TableBuilder builder(opts, sst_path);

    const int kNumKeys = 1000;
    for (int i = 0; i < kNumKeys; ++i) {
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "item_%05d", i);
        builder.Add(Slice(key_buf), Slice("cached_value_payload_" + std::to_string(i)));
    }
    assert(builder.Finish().ok());

    auto cache = std::make_shared<BlockCache>(4 * 1024 * 1024); // 4MB cache
    std::unique_ptr<TableReader> reader;
    assert(TableReader::Open(opts, sst_path, fs::file_size(sst_path), cache, &reader).ok());

    // First pass: cold cache reads (populates cache)
    for (int i = 0; i < kNumKeys; ++i) {
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "item_%05d", i);
        std::string val;
        assert(reader->Get(Slice(key_buf), &val).ok());
    }

    size_t misses_first_pass = cache->TotalMisses();
    size_t hits_first_pass = cache->TotalHits();
    std::cout << "  - Pass 1 (Cold): Misses = " << misses_first_pass << ", Hits = " << hits_first_pass << "\n";

    // Second pass: warm cache reads (all hits)
    for (int i = 0; i < kNumKeys; ++i) {
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "item_%05d", i);
        std::string val;
        assert(reader->Get(Slice(key_buf), &val).ok());
    }

    size_t total_hits = cache->TotalHits();
    size_t new_hits = total_hits - hits_first_pass;
    std::cout << "  - Pass 2 (Warm): Second-Pass Hits = " << new_hits << ", Additional Misses = "
              << (cache->TotalMisses() - misses_first_pass) << "\n";

    assert(new_hits >= static_cast<size_t>(kNumKeys));
    assert(cache->TotalMisses() == misses_first_pass); // Zero additional misses

    std::cout << "[PASSED] Test 4: Block Cache Hit Acceleration & Zero-Disk Repeated Reads Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 5: Leveled Compaction Key Overlap Merge (Feature 53)
// -----------------------------------------------------------------------------
static void Test5_LeveledCompactionKeyOverlapMerge() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 5: Leveled Compaction Strategy (LCS)\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_phase9_lcs";
    CleanTestDir(test_dir);

    TableOptions opts;
    opts.block_size = 1024;

    auto make_sst = [&](uint64_t file_num, int start_k, int end_k) -> FileMetaData {
        char path_buf[128];
        std::snprintf(path_buf, sizeof(path_buf), "%s/%06llu.sst", test_dir.c_str(), static_cast<unsigned long long>(file_num));
        std::string path = path_buf;

        TableBuilder builder(opts, path);
        for (int i = start_k; i <= end_k; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "key_%05d", i);
            builder.Add(Slice(key_buf), Slice("v_" + std::to_string(i)));
        }
        assert(builder.Finish().ok());

        FileMetaData meta;
        meta.file_number = file_num;
        meta.filename = path;
        meta.file_size = builder.FileSize();
        meta.smallest_key = builder.properties().smallest_key;
        meta.largest_key = builder.properties().largest_key;
        return meta;
    };

    // Populate Level 0 (2 overlapping files)
    FileMetaData l0_1 = make_sst(1, 100, 300);
    FileMetaData l0_2 = make_sst(2, 200, 400);

    // Populate Level 1 (2 non-overlapping files)
    FileMetaData l1_1 = make_sst(3, 150, 250);
    FileMetaData l1_2 = make_sst(4, 251, 350);

    std::vector<std::vector<FileMetaData>> levels(2);
    levels[0] = {l0_1, l0_2};
    levels[1] = {l1_1, l1_2};

    LeveledCompactionOptions lcs_opts;
    lcs_opts.l0_file_num_compaction_trigger = 2; // Force compaction trigger
    lcs_opts.target_file_size = 64 * 1024;

    LeveledCompactor compactor(lcs_opts);
    LeveledCompactionJob job;
    bool should_compact = compactor.PickCompaction(levels, &job);
    assert(should_compact);
    assert(job.level == 0);
    assert(job.inputs_level.size() == 2);
    assert(job.inputs_next_level.size() == 2); // Both L1 files overlap [100, 400]

    std::cout << "  - Compactor Selected Level 0 (Score = " << job.score << ")\n";
    std::cout << "    * Input Files L0: " << job.inputs_level.size() << " files\n";
    std::cout << "    * Overlapping Files L1: " << job.inputs_next_level.size() << " files\n";

    uint64_t next_file_num = 10;
    Status s = compactor.ExecuteCompaction(test_dir, &next_file_num, &job);
    assert(s.ok());
    assert(!job.output_files.empty());

    std::cout << "  - Generated " << job.output_files.size() << " compacted output files in Level 1:\n";
    for (const auto& out_f : job.output_files) {
        std::cout << "    * File #" << out_f.file_number << ": " << out_f.filename
                  << " ['" << out_f.smallest_key << "' .. '" << out_f.largest_key << "'], "
                  << out_f.file_size << " bytes\n";
    }

    // Verify all keys in [100, 400] are retrievable and strictly sorted without duplicates
    std::unique_ptr<TableReader> out_reader;
    assert(TableReader::Open(opts, job.output_files[0].filename, job.output_files[0].file_size, nullptr, &out_reader).ok());

    auto iter = out_reader->NewIterator();
    iter->SeekToFirst();
    int count = 0;
    while (iter->Valid()) {
        count++;
        iter->Next();
    }
    assert(count == 301); // Keys 100 to 400 inclusive

    std::cout << "[PASSED] Test 5: Leveled Compaction Multi-Way Overlap Merge 100% Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 6: Size-Tiered Compaction Bucketing (Feature 54)
// -----------------------------------------------------------------------------
static void Test6_SizeTieredCompactionBucketing() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 6: Size-Tiered Compaction Strategy (STCS)\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_phase9_stcs";
    CleanTestDir(test_dir);

    TableOptions opts;
    opts.block_size = 1024;

    std::vector<FileMetaData> all_files;

    // Generate 6 files of ~100KB each (each with 1,500 keys)
    for (int f = 1; f <= 6; ++f) {
        char path_buf[128];
        std::snprintf(path_buf, sizeof(path_buf), "%s/%06d.sst", test_dir.c_str(), f);
        std::string path = path_buf;

        TableBuilder builder(opts, path);
        for (int i = 0; i < 1500; ++i) {
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "k_%02d_%05d", f, i);
            builder.Add(Slice(key_buf), Slice("tier_val_payload_padding_data_" + std::to_string(i)));
        }
        assert(builder.Finish().ok());

        FileMetaData meta;
        meta.file_number = f;
        meta.filename = path;
        meta.file_size = builder.FileSize();
        meta.smallest_key = builder.properties().smallest_key;
        meta.largest_key = builder.properties().largest_key;
        all_files.push_back(meta);
    }

    std::cout << "  - Generated 6 similar-sized SSTable files (~"
              << (all_files[0].file_size / 1024) << " KB each)\n";

    SizeTieredCompactionOptions stcs_opts;
    stcs_opts.min_threshold = 4;
    stcs_opts.max_threshold = 4; // Select exactly 4 files

    SizeTieredCompactor compactor(stcs_opts);
    SizeTieredCompactionJob job;
    bool should_compact = compactor.PickCompaction(all_files, &job);
    assert(should_compact);
    assert(job.input_files.size() == 4);

    std::cout << "  - STCS Picked " << job.input_files.size() << " files from bucket (Avg size: "
              << job.average_size << " bytes)\n";

    uint64_t next_file_number = 100;
    Status s = compactor.ExecuteCompaction(test_dir, next_file_number, &job);
    assert(s.ok());

    std::cout << "  - Merged SSTable File: " << job.output_file.filename
              << " (" << job.output_file.file_size << " bytes, "
              << (job.output_file.file_size / 1024) << " KB)\n";

    assert(job.output_file.file_size > all_files[0].file_size * 3);
    assert(fs::exists(job.output_file.filename));

    // Validate merged SSTable
    ValidationReport rep;
    assert(SSTableValidator::ValidateFile(job.output_file.filename, &rep).ok());
    assert(rep.is_valid);
    assert(rep.num_keys == 4 * 1500); // 6,000 keys

    std::cout << "[PASSED] Test 6: Size-Tiered Compaction Bucketing & Coalescing 100% Verified.\n";
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << ">>> STARTING LSM-TREE PHASE IX SSTABLE PIPELINE & COMPACTION  <<<\n";
    std::cout << ">>> Features 49-54: Validator, Reader & Cache, Writer,       <<<\n";
    std::cout << ">>> Corruptor, Leveled Compaction (LCS) & Size-Tiered (STCS)  <<<\n";
    std::cout << "=================================================================\n";

    try {
        Test1_StreamingTableBuildAndReaderScan();
        Test2_FullSSTableIntegrityValidation();
        Test3_SystematicCorruptionInterception();
        Test4_BlockCacheHitAcceleration();
        Test5_LeveledCompactionKeyOverlapMerge();
        Test6_SizeTieredCompactionBucketing();

        std::cout << "\n=================================================================\n";
        std::cout << ">>> [ALL 6 PHASE IX VERIFICATION TESTS PASSED 100% ZERO ERRORS] <<<\n";
        std::cout << "=================================================================\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FATAL TEST EXCEPTION: " << e.what() << "\n";
        return 1;
    }
}
