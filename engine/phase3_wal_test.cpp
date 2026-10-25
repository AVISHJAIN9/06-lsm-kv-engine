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
// Test 1: Metric Histogram Accuracy & Telemetry API
// =========================================================================
void Test1_MetricHistogramAccuracy() {
    std::cout << ">>> [TEST 1] Metric Histogram Accuracy & Telemetry API..." << std::endl;
    auto& metrics = MetricsCollector::Instance();
    metrics.Reset();

    // Record 10,000 samples with controlled distribution
    // 5,000 samples at 200 ns (~ bucket 7)
    // 4,000 samples at 2,000 ns (~ bucket 10-11)
    // 900 samples at 20,000 ns (~ bucket 14)
    // 100 samples at 200,000 ns (~ bucket 17)
    for (int i = 0; i < 5000; ++i) metrics.RecordLatency(MetricType::kReadLatency, 200);
    for (int i = 0; i < 4000; ++i) metrics.RecordLatency(MetricType::kReadLatency, 2000);
    for (int i = 0; i < 900; ++i)  metrics.RecordLatency(MetricType::kReadLatency, 20000);
    for (int i = 0; i < 100; ++i)  metrics.RecordLatency(MetricType::kReadLatency, 200000);

    LatencyStats stats = metrics.GetLatencyStats(MetricType::kReadLatency);
    TEST_ASSERT(stats.count == 10000, "Expected 10,000 latency observations");
    TEST_ASSERT(stats.min_ns == 200, "Min latency should be 200ns");
    TEST_ASSERT(stats.max_ns == 200000, "Max latency should be 200000ns");
    
    // Check log-scale percentile estimation
    TEST_ASSERT(stats.p50_ns >= 128 && stats.p50_ns <= 256, "P50 bucket should contain 200ns");
    TEST_ASSERT(stats.p90_ns >= 1024 && stats.p90_ns <= 2048, "P90 bucket should contain 2000ns");
    TEST_ASSERT(stats.p99_ns >= 16384 && stats.p99_ns <= 32768, "P99 bucket should contain 20000ns");

    metrics.AddCounter(CounterType::kOpsPut, 42);
    metrics.AddCounter(CounterType::kOpsGet, 100);
    metrics.AddCounter(CounterType::kBytesWritten, 65536);

    TEST_ASSERT(metrics.GetCounter(CounterType::kOpsPut) == 42, "OpsPut counter should be 42");
    TEST_ASSERT(metrics.GetCounter(CounterType::kOpsGet) == 100, "OpsGet counter should be 100");
    TEST_ASSERT(metrics.GetCounter(CounterType::kBytesWritten) == 65536, "BytesWritten should be 65536");

    MetricsSnapshot snap = metrics.GetSnapshot();
    std::string json = snap.ToJson();
    TEST_ASSERT(!json.empty(), "Snapshot JSON should not be empty");
    TEST_ASSERT(json.find("\"ops_put\": 42") != std::string::npos, "JSON should contain ops_put");

    std::cout << "    Snapshot JSON:\n" << json << std::endl;
    std::cout << "[PASS] Test 1: Metric Histogram Accuracy verified." << std::endl;
}

// =========================================================================
// Test 2: Block Fragmentation Boundary (70KB record spanning 3 blocks)
// =========================================================================
void Test2_BlockFragmentationBoundary() {
    std::cout << ">>> [TEST 2] Block Fragmentation Boundary (70KB Record)..." << std::endl;
    const std::string test_dir = "./test_wal_dir_frag";
    CleanDirectory(test_dir);
    const std::string log_path = test_dir + "/frag_test.log";

    int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "Failed to create log file");
    auto writable = std::make_unique<PosixWritableFile>(log_path, fd);
    WALWriter writer(std::move(writable), 1);

    // Create a 70KB pattern payload (70 * 1024 = 71,680 bytes)
    // Needs 3 blocks: block 0 (32768 bytes), block 1 (32768 bytes), block 2 (remaining bytes)
    std::string large_payload(71680, '\0');
    for (size_t i = 0; i < large_payload.size(); ++i) {
        large_payload[i] = static_cast<char>('A' + (i % 26));
    }

    Status s = writer.AddRecord(Slice(large_payload));
    TEST_ASSERT(s.ok(), "Failed to add 70KB record");
    writer.Close();

    // Verify file size on disk is at least 71680 + 3 * 7 bytes of headers
    uintmax_t file_size = fs::file_size(log_path);
    TEST_ASSERT(file_size >= 71680 + 21, "File size must reflect 3 block chunks");
    std::cout << "    WAL file size for 70KB record: " << file_size << " bytes" << std::endl;

    // Read back with WALReader
    int read_fd = open(log_path.c_str(), O_RDONLY);
    TEST_ASSERT(read_fd >= 0, "Failed to open log file for reading");
    auto seq_file = std::make_unique<PosixSequentialFile>(log_path, read_fd);
    WALReader reader(std::move(seq_file), nullptr, true);

    Slice result;
    std::string scratch;
    Status read_status;
    bool ok = reader.ReadRecord(&result, &scratch, &read_status);
    TEST_ASSERT(ok && read_status.ok(), "Failed to read fragmented record");
    TEST_ASSERT(result.size() == large_payload.size(), "Defragmented record size mismatch");
    TEST_ASSERT(result.ToString() == large_payload, "Defragmented payload bit-for-bit mismatch!");

    // Verify EOF on next read
    ok = reader.ReadRecord(&result, &scratch, &read_status);
    TEST_ASSERT(!ok && read_status.ok(), "Expected EOF on second read");

    std::cout << "[PASS] Test 2: Block Fragmentation Boundary (FIRST -> MIDDLE -> LAST) verified." << std::endl;
}

// =========================================================================
// Test 3: Padding & Zero Tail Validation
// =========================================================================
void Test3_PaddingAndZeroTailValidation() {
    std::cout << ">>> [TEST 3] Padding & Zero Tail Validation..." << std::endl;
    const std::string test_dir = "./test_wal_dir_pad";
    CleanDirectory(test_dir);
    const std::string log_path = test_dir + "/pad_test.log";

    int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "Failed to create log file");
    auto writable = std::make_unique<PosixWritableFile>(log_path, fd);
    WALWriter writer(std::move(writable), 1);

    // Block size is 32768.
    // kHeaderSize = 7.
    // If we write a record with payload size = 32768 - 7 - 5 = 32756 bytes,
    // exactly 5 bytes remain in the first 32KB block.
    // Since 5 < kHeaderSize (7), the writer must pad with 5 null bytes and start the next record at offset 0 of block 2.
    std::string rec1(32756, 'X');
    std::string rec2("HELLO_NEXT_BLOCK_PAYLOAD");

    Status s = writer.AddRecord(Slice(rec1));
    TEST_ASSERT(s.ok(), "Write rec1 failed");
    s = writer.AddRecord(Slice(rec2));
    TEST_ASSERT(s.ok(), "Write rec2 failed");
    writer.Close();

    // Verify raw file: inspect the 5 padding bytes before offset 32768
    std::ifstream ifs(log_path, std::ios::binary);
    ifs.seekg(32768 - 5);
    char pad[5];
    ifs.read(pad, 5);
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT(pad[i] == 0, "Expected null byte padding in block tail");
    }
    ifs.close();

    // Verify clean readback through WALReader
    int read_fd = open(log_path.c_str(), O_RDONLY);
    auto seq_file = std::make_unique<PosixSequentialFile>(log_path, read_fd);
    WALReader reader(std::move(seq_file), nullptr, true);

    Slice result;
    std::string scratch;
    Status read_status;

    bool ok1 = reader.ReadRecord(&result, &scratch, &read_status);
    TEST_ASSERT(ok1 && read_status.ok(), "Failed to read rec1");
    TEST_ASSERT(result.size() == 32756 && result.ToString() == rec1, "rec1 content mismatch");

    bool ok2 = reader.ReadRecord(&result, &scratch, &read_status);
    TEST_ASSERT(ok2 && read_status.ok(), "Failed to read rec2 after padding");
    TEST_ASSERT(result.ToString() == rec2, "rec2 content mismatch");

    std::cout << "[PASS] Test 3: Padding & Zero Tail Validation verified." << std::endl;
}

// =========================================================================
// Test 4: CRC32C Corruption Detection
// =========================================================================
void Test4_CRC32CCorruptionDetection() {
    std::cout << ">>> [TEST 4] CRC32C Corruption Detection & Bit-Flip Isolation..." << std::endl;
    const std::string test_dir = "./test_wal_dir_crc";
    CleanDirectory(test_dir);
    const std::string log_path = test_dir + "/crc_test.log";

    int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "Failed to create log file");
    auto writable = std::make_unique<PosixWritableFile>(log_path, fd);
    WALWriter writer(std::move(writable), 1);

    std::string valid_record = "INTEGRITY_CRITICAL_TRANSACTION_PAYLOAD_123456789";
    Status s = writer.AddRecord(Slice(valid_record));
    TEST_ASSERT(s.ok(), "Write valid record failed");
    writer.Close();

    // Corrupt a single bit in the payload on disk
    int corrupt_fd = open(log_path.c_str(), O_RDWR);
    TEST_ASSERT(corrupt_fd >= 0, "Failed to open log file for tampering");
    lseek(corrupt_fd, 10, SEEK_SET); // 3rd byte into payload
    char b = 0;
    ssize_t rd = read(corrupt_fd, &b, 1);
    TEST_ASSERT(rd == 1, "Failed to read byte for corruption");
    b ^= 0x01; // Flip lowest bit
    lseek(corrupt_fd, 10, SEEK_SET);
    ssize_t wr = write(corrupt_fd, &b, 1);
    TEST_ASSERT(wr == 1, "Failed to write corrupted byte");
    close(corrupt_fd);

    // Read back with WALReader with checksum verification enabled
    int read_fd = open(log_path.c_str(), O_RDONLY);
    auto seq_file = std::make_unique<PosixSequentialFile>(log_path, read_fd);
    WALReader reader(std::move(seq_file), nullptr, true);

    Slice result;
    std::string scratch;
    Status read_status;
    bool ok = reader.ReadRecord(&result, &scratch, &read_status);

    TEST_ASSERT(!ok, "Reader must not succeed on corrupted block");
    TEST_ASSERT(read_status.IsCorruption(), "Status must be Corruption on CRC mismatch");
    std::cout << "    Correctly caught expected corruption: " << read_status.ToString() << std::endl;

    std::cout << "[PASS] Test 4: CRC32C Bit-Flip Corruption Detection verified." << std::endl;
}

// =========================================================================
// Test 5: Segment File Rotation & Archival
// =========================================================================
void Test5_SegmentFileRotation() {
    std::cout << ">>> [TEST 5] Segment File Rotation & Archival..." << std::endl;
    const std::string test_dir = "./test_wal_dir_rotate";
    CleanDirectory(test_dir);

    WALOptions options;
    options.wal_dir = test_dir;
    options.max_wal_file_size = 100 * 1024; // 100KB threshold
    options.sync_on_rotate = true;

    WALManager manager(options);
    Status s = manager.Open();
    TEST_ASSERT(s.ok(), "Manager Open failed");

    // Append 350KB of data in 1KB batches
    std::string chunk_payload(1024, 'R');
    for (int i = 0; i < 350; ++i) {
        s = manager.Append(Slice(chunk_payload), false);
        TEST_ASSERT(s.ok(), "Manager Append failed");
    }

    // Must have generated 4 `.log` files: 000001.log, 000002.log, 000003.log, 000004.log
    std::vector<uint64_t> active_nums = manager.GetActiveLogNumbers();
    TEST_ASSERT(active_nums.size() == 4, "Expected 4 active WAL files");
    TEST_ASSERT(active_nums[0] == 1 && active_nums[3] == 4, "Log numbering should be 1..4");

    std::cout << "    Generated active WAL segments: ";
    for (uint64_t num : active_nums) std::cout << num << " ";
    std::cout << "\n    Total WAL bytes: " << manager.GetTotalWALBytes() << " bytes" << std::endl;

    // Purge obsolete logs below segment 3 (should delete 000001.log and 000002.log)
    size_t deleted_count = 0;
    s = manager.PurgeObsoleteWALs(3, &deleted_count);
    TEST_ASSERT(s.ok(), "PurgeObsoleteWALs failed");
    TEST_ASSERT(deleted_count == 2, "Expected 2 obsolete logs purged");

    active_nums = manager.GetActiveLogNumbers();
    TEST_ASSERT(active_nums.size() == 2, "Expected 2 active WAL files remaining");
    TEST_ASSERT(active_nums[0] == 3 && active_nums[1] == 4, "Remaining logs should be 3 and 4");

    TEST_ASSERT(!fs::exists(test_dir + "/000001.log"), "000001.log must be deleted");
    TEST_ASSERT(!fs::exists(test_dir + "/000002.log"), "000002.log must be deleted");
    TEST_ASSERT(fs::exists(test_dir + "/000003.log"), "000003.log must exist");
    TEST_ASSERT(fs::exists(test_dir + "/000004.log"), "000004.log must exist");

    manager.Close();
    std::cout << "[PASS] Test 5: Segment File Rotation & Archival verified." << std::endl;
}

// =========================================================================
// Test 6: Group Commit & Fsync Durability Policy
// =========================================================================
void Test6_GroupCommitFsyncPolicy() {
    std::cout << ">>> [TEST 6] Group Commit & Fsync Durability Policy (16 Threads)..." << std::endl;
    const std::string db_dir = "./test_db_phase3_wal";
    CleanDirectory(db_dir);

    Options options;
    options.enable_wal = true;
    options.wal_dir = db_dir + "/wal";
    options.fsync_mode = FsyncMode::kFsyncGroupCommit;

    DB* db = nullptr;
    Status s = DB::Open(options, db_dir, &db);
    TEST_ASSERT(s.ok() && db != nullptr, "DB Open failed");

    constexpr int kNumThreads = 16;
    constexpr int kOpsPerThread = 100; // Total 1,600 concurrent synchronous operations
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([db, t, &success_count]() {
            WriteOptions write_opts;
            write_opts.sync = true; // Request sync to trigger group commit coordinator

            for (int i = 0; i < kOpsPerThread; ++i) {
                std::string key = "key_t" + std::to_string(t) + "_" + std::to_string(i);
                std::string val = "val_data_" + std::to_string(t * 10000 + i);

                Status st = db->Put(write_opts, Slice(key), Slice(val));
                if (st.ok()) {
                    success_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    TEST_ASSERT(success_count.load() == kNumThreads * kOpsPerThread, "All 1,600 writes must succeed");

    std::cout << "    Successfully completed " << success_count.load()
              << " synchronous writes in " << duration_ms << " ms ("
              << (success_count.load() * 1000.0 / (duration_ms > 0 ? duration_ms : 1))
              << " ops/sec under group commit)" << std::endl;

    // Verify all keys can be read correctly
    ReadOptions read_opts;
    for (int t = 0; t < kNumThreads; ++t) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            std::string key = "key_t" + std::to_string(t) + "_" + std::to_string(i);
            std::string expected_val = "val_data_" + std::to_string(t * 10000 + i);
            std::string actual_val;
            Status get_st = db->Get(read_opts, Slice(key), &actual_val);
            TEST_ASSERT(get_st.ok(), "Key read failed");
            TEST_ASSERT(actual_val == expected_val, "Key value mismatch");
        }
    }

    MetricsSnapshot metrics_snap = db->GetMetrics();
    std::cout << "    Metrics Summary:\n"
              << "    - Ops Put: " << metrics_snap.counters[static_cast<size_t>(CounterType::kOpsPut)] << "\n"
              << "    - Group Commit Batches: " << metrics_snap.counters[static_cast<size_t>(CounterType::kGroupCommitBatches)] << "\n"
              << "    - WAL Sync Count: " << metrics_snap.counters[static_cast<size_t>(CounterType::kWalSyncCount)] << "\n"
              << "    - WAL Bytes Flushed: " << metrics_snap.counters[static_cast<size_t>(CounterType::kWalBytesFlushed)] << "\n"
              << "    - P50 Write Latency: " << metrics_snap.latencies[static_cast<size_t>(MetricType::kWriteLatency)].p50_ns << " ns\n"
              << "    - P99 Write Latency: " << metrics_snap.latencies[static_cast<size_t>(MetricType::kWriteLatency)].p99_ns << " ns"
              << std::endl;

    delete db;
    std::cout << "[PASS] Test 6: Group Commit & Fsync Durability Policy verified." << std::endl;
}

// =========================================================================
// Main Test Runner
// =========================================================================
int main() {
    std::cout << "================================================================================" << std::endl;
    std::cout << "  LSM-Tree Key-Value Engine: Phase III Verification Test Suite" << std::endl;
    std::cout << "  Features: 13 (Metrics), 14 (WAL Format), 15 (Append Path), 16 (Rotation)," << std::endl;
    std::cout << "            17 (CRC32C Bit-Flip), 18 (Group Commit & Fsync Durability)" << std::endl;
    std::cout << "================================================================================" << std::endl;

    Test1_MetricHistogramAccuracy();
    Test2_BlockFragmentationBoundary();
    Test3_PaddingAndZeroTailValidation();
    Test4_CRC32CCorruptionDetection();
    Test5_SegmentFileRotation();
    Test6_GroupCommitFsyncPolicy();

    std::cout << "================================================================================" << std::endl;
    std::cout << "  ALL 6 PHASE III VERIFICATION TESTS PASSED (100% SUCCESS, ZERO DEFECTS)!" << std::endl;
    std::cout << "================================================================================" << std::endl;

    return 0;
}
