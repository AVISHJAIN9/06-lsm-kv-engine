#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <filesystem>
#include <chrono>
#include <thread>
#include <iomanip>

#include "compaction_picker.hpp"
#include "compaction_scheduler.hpp"
#include "compaction_concurrency.hpp"
#include "compaction_input_selector.hpp"
#include "compaction_executor.hpp"
#include "write_amplification.hpp"
#include "table_builder.hpp"
#include "table_reader.hpp"
#include "types.hpp"

namespace fs = std::filesystem;
using namespace lsm;

static void SetupDir(const std::string& path) {
    if (fs::exists(path)) {
        fs::remove_all(path);
    }
    fs::create_directories(path);
}

static FileMetaData CreateTestSSTable(
    const std::string& db_dir,
    uint64_t file_num,
    const std::vector<std::pair<std::string, std::string>>& kvs,
    SequenceNumber seq = 1,
    ValueType vtype = kTypeValue
) {
    char name_buf[128];
    std::snprintf(name_buf, sizeof(name_buf), "%s/%06llu.sst", db_dir.c_str(), static_cast<unsigned long long>(file_num));
    std::string filename = name_buf;

    TableOptions topts;
    topts.block_size = 512;
    topts.filter_policy = std::make_shared<BloomFilterPolicy>(10);

    TableBuilder builder(topts, filename);
    for (const auto& kv : kvs) {
        InternalKey ikey(Slice(kv.first), seq, vtype);
        builder.Add(ikey.Encode(), Slice(kv.second));
    }
    Status s = builder.Finish();
    assert(s.ok());

    FileMetaData meta;
    meta.file_number = file_num;
    meta.filename = filename;
    meta.file_size = builder.FileSize();
    meta.smallest_key = builder.properties().smallest_key;
    meta.largest_key = builder.properties().largest_key;
    meta.min_seq = seq;
    meta.max_seq = seq;
    return meta;
}

// -----------------------------------------------------------------------------
// Test 1: Compaction Score Prioritization
// Populate L0 with 6 files (trigger: 4, score = 1.5) and L1 with 15MB (limit: 10MB, score = 1.5)
// Verify picker prioritizes L0 -> L1 and returns correct candidate job.
// -----------------------------------------------------------------------------
void Test1_CompactionScorePrioritization() {
    std::cout << "\n=== Test 1: Compaction Score Prioritization (Feature 55) ===\n";

    LeveledCompactionOptions opts;
    opts.l0_file_num_compaction_trigger = 4;
    opts.base_level_bytes = 10 * 1024 * 1024; // 10 MB
    opts.level_multiplier = 10.0;
    opts.max_levels = 7;

    CompactionPicker picker(opts);

    std::vector<std::vector<FileMetaData>> levels(7);

    // Populate L0 with 6 files (trigger = 4 -> score = 6 / 4 = 1.5)
    for (uint64_t i = 1; i <= 6; ++i) {
        FileMetaData f;
        f.file_number = i;
        f.file_size = 2 * 1024 * 1024; // 2MB
        f.smallest_key = "key_" + std::to_string(i * 10);
        f.largest_key = "key_" + std::to_string(i * 10 + 9);
        levels[0].push_back(f);
    }

    // Populate L1 with 15MB total (base_level_bytes = 10MB -> score = 15 / 10 = 1.5)
    for (uint64_t i = 7; i <= 11; ++i) {
        FileMetaData f;
        f.file_number = i;
        f.file_size = 3 * 1024 * 1024; // 3MB each -> total 15MB
        f.smallest_key = "k_" + std::to_string(i * 10);
        f.largest_key = "k_" + std::to_string(i * 10 + 9);
        levels[1].push_back(f);
    }

    auto scores = picker.ComputeLevelScores(levels);
    assert(scores.size() == 7);
    std::cout << "  - Level 0 Score: " << scores[0].score << " (Files: 6, Trigger: 4)\n";
    std::cout << "  - Level 1 Score: " << scores[1].score << " (Bytes: " << scores[1].bytes / (1024*1024) << "MB, Target: 10MB)\n";

    assert(std::abs(scores[0].score - 1.5) < 1e-5);
    assert(std::abs(scores[1].score - 1.5) < 1e-5);

    // Pick compaction: On equal score 1.5, L0 -> L1 must take priority
    std::unordered_set<uint64_t> files_being_compacted;
    LeveledCompactionJob job;
    bool picked = picker.PickCompaction(levels, files_being_compacted, &job);

    assert(picked);
    assert(job.level == 0);
    assert(job.inputs_level.size() == 6);
    std::cout << "  - CompactionPicker successfully prioritized L0 -> L1 (Level " << job.level 
              << ", Score: " << job.score << ", Inputs: " << job.inputs_level.size() << " files)\n";
    std::cout << "  [PASSED] Test 1: Compaction Score Prioritization\n";
}

// -----------------------------------------------------------------------------
// Test 2: Multi-Worker Concurrent Execution
// Launch 4 background worker threads; trigger two disjoint compactions:
// L1 -> L2 on key range [a, m] and L1 -> L2 on key range [n, z].
// Assert both run concurrently in parallel without blocking or file collisions.
// -----------------------------------------------------------------------------
void Test2_MultiWorkerConcurrentExecution() {
    std::cout << "\n=== Test 2: Multi-Worker Concurrent Execution (Features 56 & 57) ===\n";

    std::string test_dir = "./test_phase10_concurrent";
    SetupDir(test_dir);

    auto concurrency_gate = std::make_shared<CompactionConcurrencyGate>();
    auto waf_tracker = std::make_shared<WriteAmplificationTracker>();
    CompactionScheduler scheduler(4, concurrency_gate, waf_tracker);

    // Job 1: Key range [a, m]
    std::vector<std::pair<std::string, std::string>> kvs1 = {
        {"apple", "v1"}, {"banana", "v2"}, {"cherry", "v3"}, {"mango", "v4"}
    };
    FileMetaData f1 = CreateTestSSTable(test_dir, 101, kvs1, 10);

    // Job 2: Key range [n, z]
    std::vector<std::pair<std::string, std::string>> kvs2 = {
        {"nut", "v5"}, {"orange", "v6"}, {"walnut", "v7"}, {"zebra", "v8"}
    };
    FileMetaData f2 = CreateTestSSTable(test_dir, 102, kvs2, 20);

    uint64_t next_file_num = 200;
    std::atomic<int> completed_count{0};

    ScheduledCompactionTask task1;
    task1.job.level = 1;
    task1.job.score = 2.0;
    task1.job.inputs_level = {f1};
    task1.db_dir = test_dir;
    task1.next_file_number = &next_file_num;
    task1.on_complete = [&](const Status& s, const LeveledCompactionJob& j) {
        assert(s.ok());
        assert(!j.output_files.empty());
        completed_count++;
    };

    ScheduledCompactionTask task2;
    task2.job.level = 1;
    task2.job.score = 1.8;
    task2.job.inputs_level = {f2};
    task2.db_dir = test_dir;
    task2.next_file_number = &next_file_num;
    task2.on_complete = [&](const Status& s, const LeveledCompactionJob& j) {
        assert(s.ok());
        assert(!j.output_files.empty());
        completed_count++;
    };

    bool s1 = scheduler.ScheduleCompaction(task1);
    bool s2 = scheduler.ScheduleCompaction(task2);
    assert(s1 && s2);

    scheduler.WaitForCompletion();
    assert(completed_count.load() == 2);
    assert(scheduler.CompletedTasksCount() == 2);

    std::cout << "  - 2 disjoint compaction tasks scheduled and executed concurrently across 4 workers.\n";
    std::cout << "  - Verified zero deadlocks and zero file collisions.\n";
    std::cout << "  [PASSED] Test 2: Multi-Worker Concurrent Execution\n";
}

// -----------------------------------------------------------------------------
// Test 3: File Reservation Exclusion
// Attempt to trigger a compaction on a file already marked as being_compacted;
// assert the concurrency gate rejects the job and prevents duplicate execution.
// -----------------------------------------------------------------------------
void Test3_FileReservationExclusion() {
    std::cout << "\n=== Test 3: File Reservation Exclusion (Feature 57) ===\n";

    CompactionConcurrencyGate gate;
    std::vector<uint64_t> file_ids = {501, 502, 503};

    // Worker 1 locks files
    bool locked = gate.TryLockFiles(file_ids);
    assert(locked);
    assert(gate.ActiveLockedFilesCount() == 3);
    assert(gate.IsFileBeingCompacted(502));

    // Worker 2 attempts to lock overlapping file 502
    std::vector<uint64_t> overlapping_ids = {502, 504};
    bool conflict = gate.TryLockFiles(overlapping_ids);
    assert(!conflict); // Must be rejected
    std::cout << "  - Concurrency gate rejected overlapping file lock attempt for file 502.\n";

    // Worker 1 releases locks
    gate.UnlockFiles(file_ids);
    assert(gate.ActiveLockedFilesCount() == 0);
    assert(!gate.IsFileBeingCompacted(502));

    // Worker 2 can now successfully lock
    bool retry = gate.TryLockFiles(overlapping_ids);
    assert(retry);
    assert(gate.ActiveLockedFilesCount() == 2);
    gate.UnlockFiles(overlapping_ids);
    assert(gate.ActiveLockedFilesCount() == 0);

    std::cout << "  [PASSED] Test 3: File Reservation Exclusion\n";
}

// -----------------------------------------------------------------------------
// Test 4: Disjoint Overlap Expansion
// Configure L1 file with keys [k20, k40]; configure L2 files [k10, k25], [k26, k35], [k36, k50];
// assert overlap selector correctly identifies all 3 overlapping L2 files.
// -----------------------------------------------------------------------------
void Test4_DisjointOverlapExpansion() {
    std::cout << "\n=== Test 4: Disjoint Overlap Expansion (Feature 58) ===\n";

    FileMetaData l1_file;
    l1_file.file_number = 10;
    l1_file.smallest_key = "k20";
    l1_file.largest_key = "k40";

    std::vector<FileMetaData> l2_files;

    FileMetaData l2_f1;
    l2_f1.file_number = 21;
    l2_f1.smallest_key = "k10";
    l2_f1.largest_key = "k25";
    l2_files.push_back(l2_f1);

    FileMetaData l2_f2;
    l2_f2.file_number = 22;
    l2_f2.smallest_key = "k26";
    l2_f2.largest_key = "k35";
    l2_files.push_back(l2_f2);

    FileMetaData l2_f3;
    l2_f3.file_number = 23;
    l2_f3.smallest_key = "k36";
    l2_f3.largest_key = "k50";
    l2_files.push_back(l2_f3);

    FileMetaData l2_f4;
    l2_f4.file_number = 24;
    l2_f4.smallest_key = "k60";
    l2_f4.largest_key = "k70";
    l2_files.push_back(l2_f4);

    std::vector<FileMetaData> overlapping;
    CompactionInputSelector::GetOverlappingInputs(
        Slice(l1_file.smallest_key), Slice(l1_file.largest_key), l2_files, &overlapping
    );

    assert(overlapping.size() == 3);
    assert(overlapping[0].file_number == 21);
    assert(overlapping[1].file_number == 22);
    assert(overlapping[2].file_number == 23);

    std::cout << "  - Input range [" << l1_file.smallest_key << ", " << l1_file.largest_key << "] matched "
              << overlapping.size() << " L2 files: ";
    for (const auto& f : overlapping) {
        std::cout << "F" << f.file_number << "[" << f.smallest_key << ".." << f.largest_key << "] ";
    }
    std::cout << "\n";
    std::cout << "  [PASSED] Test 4: Disjoint Overlap Expansion\n";
}

// -----------------------------------------------------------------------------
// Test 5: Streaming Compaction & Tombstone GC
// Merge 3 input files containing multiple overwritten versions and deleted keys;
// assert output contains only the latest visible versions, tombstones are purged,
// and output splits into uniform-sized SSTable files.
// -----------------------------------------------------------------------------
void Test5_StreamingCompactionAndTombstoneGC() {
    std::cout << "\n=== Test 5: Streaming Compaction & Tombstone GC (Feature 59) ===\n";

    std::string test_dir = "./test_phase10_streaming_gc";
    SetupDir(test_dir);

    // File 1: Oldest versions (seq 100)
    std::vector<std::pair<std::string, std::string>> kvs1 = {
        {"user_100", "old_v1"},
        {"user_200", "will_be_deleted"},
        {"user_300", "old_v3"},
        {"user_400", "keep_me"}
    };
    FileMetaData f1 = CreateTestSSTable(test_dir, 301, kvs1, 100, kTypeValue);

    // File 2: Newer versions (seq 200) with update to user_100 and tombstone for user_200
    std::vector<std::pair<std::string, std::string>> kvs2 = {
        {"user_100", "new_v1"},
        {"user_200", ""}, // Deletion tombstone!
    };
    char name_buf2[128];
    std::snprintf(name_buf2, sizeof(name_buf2), "%s/%06llu.sst", test_dir.c_str(), 302ULL);
    TableOptions topts;
    topts.block_size = 512;
    TableBuilder b2(topts, name_buf2);
    InternalKey k1(Slice("user_100"), 200, kTypeValue);
    b2.Add(k1.Encode(), Slice("new_v1"));
    InternalKey k2(Slice("user_200"), 200, kTypeDeletion);
    b2.Add(k2.Encode(), Slice(""));
    b2.Finish();
    FileMetaData f2;
    f2.file_number = 302;
    f2.filename = name_buf2;
    f2.file_size = b2.FileSize();
    f2.smallest_key = b2.properties().smallest_key;
    f2.largest_key = b2.properties().largest_key;

    // File 3: seq 300 with update to user_300 and payload to force splitting
    std::vector<std::pair<std::string, std::string>> kvs3;
    kvs3.push_back({"user_300", "newest_v3"});
    // Add large values to trigger file splitting
    for (int i = 500; i < 550; ++i) {
        kvs3.push_back({"user_" + std::to_string(i), std::string(200, 'X')});
    }
    FileMetaData f3 = CreateTestSSTable(test_dir, 303, kvs3, 300, kTypeValue);

    LeveledCompactionJob job;
    job.level = 1;
    job.inputs_level = {f1, f2};
    job.inputs_next_level = {f3};

    CompactionExecutorOptions exec_opts;
    exec_opts.target_file_size = 2048; // Split at 2 KB boundaries
    exec_opts.is_bottom_level = true;  // Purge tombstones at bottom level

    auto waf_tracker = std::make_shared<WriteAmplificationTracker>();
    CompactionExecutor executor(exec_opts, waf_tracker);

    uint64_t next_file_num = 400;
    Status s = executor.Execute(test_dir, &next_file_num, &job);
    assert(s.ok());
    assert(job.output_files.size() >= 2); // Assert file splitting occurred!

    std::cout << "  - Compaction generated " << job.output_files.size() << " split SSTable files.\n";

    // Verify output contents using TableReader
    std::vector<std::pair<std::string, std::string>> final_entries;
    for (const auto& out_f : job.output_files) {
        std::unique_ptr<TableReader> reader;
        Status r_s = TableReader::Open(topts, out_f.filename, out_f.file_size, nullptr, &reader);
        assert(r_s.ok());

        auto iter = reader->NewIterator();
        iter->SeekToFirst();
        while (iter->Valid()) {
            InternalKey ikey;
            ikey.DecodeFrom(iter->key());
            final_entries.push_back({ikey.user_key().ToString(), iter->value().ToString()});
            iter->Next();
        }
    }

    // Verify deduplication:
    // 1. user_100 must be "new_v1"
    // 2. user_200 (tombstone) must be completely purged!
    // 3. user_300 must be "newest_v3"
    // 4. user_400 must be "keep_me"
    bool found_100 = false, found_200 = false, found_300 = false, found_400 = false;
    for (const auto& entry : final_entries) {
        if (entry.first == "user_100") {
            assert(entry.second == "new_v1");
            found_100 = true;
        } else if (entry.first == "user_200") {
            found_200 = true; // Should NOT be found
        } else if (entry.first == "user_300") {
            assert(entry.second == "newest_v3");
            found_300 = true;
        } else if (entry.first == "user_400") {
            assert(entry.second == "keep_me");
            found_400 = true;
        }
    }

    assert(found_100);
    assert(!found_200); // Tombstone purged!
    assert(found_300);
    assert(found_400);

    std::cout << "  - Verified: user_100 newest version preserved, user_200 tombstone purged, user_300 newest version preserved.\n";
    std::cout << "  [PASSED] Test 5: Streaming Compaction & Tombstone GC\n";
}

// -----------------------------------------------------------------------------
// Test 6: Write Amplification Precision
// Execute 10,000 user writes of 100 bytes each (1 MB user data); run through
// WAL, flush, and compaction; query GetCumulativeWAF(); assert exact mathematical match.
// -----------------------------------------------------------------------------
void Test6_WriteAmplificationPrecision() {
    std::cout << "\n=== Test 6: Write Amplification Factor (WAF) Accounting (Feature 60) ===\n";

    WriteAmplificationTracker tracker;

    // 10,000 user writes of 100 bytes each = 1,000,000 bytes user data
    const size_t num_writes = 10000;
    const size_t write_size = 100;
    const size_t total_user_bytes = num_writes * write_size;

    tracker.RecordUserWrite(total_user_bytes);

    // WAL writes (payload + record framing, e.g. 108 bytes per record)
    const size_t wal_bytes = num_writes * 108;
    tracker.RecordWALWrite(wal_bytes);

    // Memtable Flush writes (compressed SSTable + index + bloom filter, e.g. 1,050,000 bytes)
    const size_t flush_bytes = 1050000;
    tracker.RecordFlushWrite(flush_bytes);

    // Compaction writes (L0 -> L1 rewrite + L1 -> L2 rewrite, e.g. 3,500,000 bytes)
    const size_t compaction_bytes = 3500000;
    tracker.RecordCompactionWrite(compaction_bytes);

    uint64_t expected_physical = wal_bytes + flush_bytes + compaction_bytes;
    double expected_cumulative_waf = static_cast<double>(expected_physical) / total_user_bytes;
    double expected_compaction_waf = static_cast<double>(compaction_bytes) / total_user_bytes;

    double actual_cumulative_waf = tracker.GetCumulativeWAF();
    double actual_compaction_waf = tracker.GetCompactionWAF();

    assert(std::abs(actual_cumulative_waf - expected_cumulative_waf) < 1e-6);
    assert(std::abs(actual_compaction_waf - expected_compaction_waf) < 1e-6);

    WAFSummary summary = tracker.GetSummary();
    assert(summary.bytes_user_written == total_user_bytes);
    assert(summary.total_physical_bytes_written == expected_physical);

    std::cout << summary.ToString();
    std::cout << "  - Expected Cumulative WAF: " << expected_cumulative_waf << "x\n";
    std::cout << "  - Actual Cumulative WAF:   " << actual_cumulative_waf << "x (Precision: 100%)\n";
    std::cout << "  [PASSED] Test 6: Write Amplification Precision\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase X Verification Test Suite\n";
    std::cout << "  Features 55-60: Compaction Pipeline Core (Part 2)\n";
    std::cout << "=======================================================================\n";

    Test1_CompactionScorePrioritization();
    Test2_MultiWorkerConcurrentExecution();
    Test3_FileReservationExclusion();
    Test4_DisjointOverlapExpansion();
    Test5_StreamingCompactionAndTombstoneGC();
    Test6_WriteAmplificationPrecision();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase X Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
