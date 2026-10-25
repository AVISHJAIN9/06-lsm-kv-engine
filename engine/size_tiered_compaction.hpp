#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "status.hpp"
#include "leveled_compaction.hpp"
#include "table_reader.hpp"
#include "table_builder.hpp"

namespace lsm {

struct SizeTieredCompactionOptions {
    size_t min_threshold{4};
    size_t max_threshold{32};
    double bucket_low{0.5};
    double bucket_high{1.5};
    uint64_t min_sstable_size{50 * 1024};
};

struct SizeTieredCompactionJob {
    std::vector<FileMetaData> input_files;
    FileMetaData output_file;
    double average_size{0.0};
};

// -----------------------------------------------------------------------------
// SizeTieredCompactor: Groups SSTables into size tiers and merges similar-sized files
// -----------------------------------------------------------------------------
class SizeTieredCompactor {
public:
    explicit SizeTieredCompactor(SizeTieredCompactionOptions options = SizeTieredCompactionOptions{});

    // Evaluates files and picks a bucket containing >= min_threshold similar-sized files
    bool PickCompaction(
        const std::vector<FileMetaData>& all_files,
        SizeTieredCompactionJob* out_job
    ) const;

    // Merges selected bucket files into a single consolidated SSTable
    Status ExecuteCompaction(
        const std::string& db_dir,
        uint64_t next_file_number,
        SizeTieredCompactionJob* job,
        std::shared_ptr<BlockCache> block_cache = nullptr
    );

private:
    SizeTieredCompactionOptions options_;
};

} // namespace lsm
