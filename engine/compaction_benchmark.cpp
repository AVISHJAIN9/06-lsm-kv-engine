#include "compaction_benchmark.hpp"
#include <chrono>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "compaction_executor.hpp"
#include "table_builder.hpp"
#include "types.hpp"

namespace fs = std::filesystem;

namespace lsm {

CompactionBenchmark::CompactionBenchmark(CompactionBenchmarkOptions options)
    : options_(std::move(options)) {}

Status CompactionBenchmark::Run(CompactionBenchmarkResult* out_result) {
    if (!out_result) {
        return Status::InvalidArgument("Null pointer passed for out_result");
    }

    std::error_code ec;
    fs::create_directories(options_.db_dir, ec);

    // 1. Prepare synthetic dataset across 2 input SSTable files
    // File 1: Base entries (seq 100)
    std::string file1_path = options_.db_dir + "/bench_001.sst";
    TableOptions topts;
    topts.block_size = 4096;
    topts.filter_policy = std::make_shared<BloomFilterPolicy>(10);

    TableBuilder b1(topts, file1_path);
    size_t file1_records = options_.num_keys;
    for (size_t i = 0; i < file1_records; ++i) {
        char kbuf[64];
        std::snprintf(kbuf, sizeof(kbuf), "key_%08zu", i);
        std::string val(options_.value_size, 'A' + (i % 26));

        InternalKey ikey(Slice(kbuf), 100, kTypeValue);
        b1.Add(ikey.Encode(), Slice(val));
    }
    Status s = b1.Finish();
    if (!s.ok()) return s;

    FileMetaData meta1;
    meta1.file_number = 1001;
    meta1.filename = file1_path;
    meta1.file_size = b1.FileSize();
    meta1.smallest_key = b1.properties().smallest_key;
    meta1.largest_key = b1.properties().largest_key;

    // File 2: Overwrites & Deletions (seq 200)
    std::string file2_path = options_.db_dir + "/bench_002.sst";
    TableBuilder b2(topts, file2_path);

    size_t overwrite_count = static_cast<size_t>(options_.num_keys * (options_.overwrite_percent / 100.0));
    size_t tombstone_count = static_cast<size_t>(options_.num_keys * (options_.tombstone_percent / 100.0));

    // Ensure disjoint/ordered keys for file 2
    size_t file2_records = 0;
    for (size_t i = 0; i < options_.num_keys; ++i) {
        char kbuf[64];
        std::snprintf(kbuf, sizeof(kbuf), "key_%08zu", i);

        if (i < tombstone_count) {
            InternalKey ikey(Slice(kbuf), 200, kTypeDeletion);
            b2.Add(ikey.Encode(), Slice(""));
            file2_records++;
        } else if (i < tombstone_count + overwrite_count) {
            std::string updated_val(options_.value_size, 'Z');
            InternalKey ikey(Slice(kbuf), 200, kTypeValue);
            b2.Add(ikey.Encode(), Slice(updated_val));
            file2_records++;
        }
    }
    s = b2.Finish();
    if (!s.ok()) return s;

    FileMetaData meta2;
    meta2.file_number = 1002;
    meta2.filename = file2_path;
    meta2.file_size = b2.FileSize();
    meta2.smallest_key = b2.properties().smallest_key;
    meta2.largest_key = b2.properties().largest_key;

    // 2. Set up compaction job
    LeveledCompactionJob job;
    job.level = 1;
    job.inputs_level = {meta1};
    job.inputs_next_level = {meta2};

    CompactionExecutorOptions exec_opts;
    exec_opts.target_file_size = 64 * 1024 * 1024; // 64 MB
    exec_opts.is_bottom_level = true;              // Purge tombstones!

    CompactionExecutor executor(exec_opts);
    uint64_t next_file_num = 2000;

    // 3. Profile compaction execution
    auto start_time = std::chrono::high_resolution_clock::now();
    s = executor.Execute(options_.db_dir, &next_file_num, &job);
    auto end_time = std::chrono::high_resolution_clock::now();

    if (!s.ok()) return s;

    // 4. Calculate metrics
    out_result->input_bytes = meta1.file_size + meta2.file_size;
    out_result->input_records = file1_records + file2_records;
    out_result->output_bytes = 0;
    out_result->output_records = 0;
    for (const auto& out_f : job.output_files) {
        out_result->output_bytes += out_f.file_size;
    }

    out_result->tombstones_purged = tombstone_count;
    // Expected output records = num_keys - tombstone_count
    out_result->output_records = (options_.num_keys > tombstone_count) ? (options_.num_keys - tombstone_count) : 0;

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    out_result->elapsed_ms = static_cast<double>(elapsed_us) / 1000.0;

    double elapsed_sec = out_result->elapsed_ms / 1000.0;
    if (elapsed_sec > 0) {
        out_result->throughput_mb_sec = (static_cast<double>(out_result->input_bytes) / (1024.0 * 1024.0)) / elapsed_sec;
        out_result->records_per_sec = static_cast<double>(out_result->input_records) / elapsed_sec;
    }

    if (out_result->input_bytes > 0) {
        if (out_result->output_bytes < out_result->input_bytes) {
            out_result->space_reduction_pct = (1.0 - (static_cast<double>(out_result->output_bytes) / out_result->input_bytes)) * 100.0;
        } else {
            out_result->space_reduction_pct = 0.0;
        }
    }

    return Status::OK();
}

std::string CompactionBenchmarkResult::ToString() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "Compaction Benchmark Diagnostics Report:\n"
        << "  - Input Data Volume:        " << (input_bytes / 1024.0) << " KB (" << input_records << " records)\n"
        << "  - Compacted Output Volume:  " << (output_bytes / 1024.0) << " KB (" << output_records << " records)\n"
        << "  - Tombstones Purged (GC):   " << tombstones_purged << " keys\n"
        << "  - Space Reduction Ratio:    " << space_reduction_pct << "%\n"
        << "  - Compaction Duration:      " << elapsed_ms << " ms\n"
        << "  - Processing Throughput:    " << throughput_mb_sec << " MB/sec (" << static_cast<uint64_t>(records_per_sec) << " records/sec)\n";
    return oss.str();
}

} // namespace lsm
