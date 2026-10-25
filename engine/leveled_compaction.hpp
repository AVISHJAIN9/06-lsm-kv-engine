#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "table_reader.hpp"
#include "table_builder.hpp"

namespace lsm {

struct FileMetaData {
    uint64_t file_number{0};
    std::string filename;
    uint64_t file_size{0};
    std::string smallest_key;
    std::string largest_key;
    SequenceNumber min_seq{0};
    SequenceNumber max_seq{0};
};

struct LeveledCompactionOptions {
    size_t max_levels{7};
    uint64_t l0_file_num_compaction_trigger{4};
    uint64_t base_level_bytes{10 * 1024 * 1024}; // 10MB for L1
    double level_multiplier{10.0};              // 10x multiplier
    uint64_t target_file_size{2 * 1024 * 1024};  // 2MB per SSTable
};

struct LeveledCompactionJob {
    int level{-1};
    double score{0.0};
    std::vector<FileMetaData> inputs_level;     // Files from Level i
    std::vector<FileMetaData> inputs_next_level;// Overlapping files from Level i+1
    std::vector<FileMetaData> output_files;     // Generated files in Level i+1
};

// -----------------------------------------------------------------------------
// LeveledCompactor: Evaluates level scoring, selects overlapping ranges, and executes merge
// -----------------------------------------------------------------------------
class LeveledCompactor {
public:
    explicit LeveledCompactor(LeveledCompactionOptions options);

    // Evaluates all level scores Si = Bytes(Li) / Target(Li) and picks the highest
    bool PickCompaction(
        const std::vector<std::vector<FileMetaData>>& levels,
        LeveledCompactionJob* out_job
    ) const;

    // Executes multi-way merge sort of input files and produces sorted non-overlapping output files
    Status ExecuteCompaction(
        const std::string& db_dir,
        uint64_t* next_file_number,
        LeveledCompactionJob* job,
        std::shared_ptr<BlockCache> block_cache = nullptr
    );

    uint64_t TargetBytesForLevel(int level) const;

private:
    void FindOverlappingFiles(
        const std::string& smallest,
        const std::string& largest,
        const std::vector<FileMetaData>& next_level_files,
        std::vector<FileMetaData>* out_overlapping
    ) const;

    LeveledCompactionOptions options_;
};

} // namespace lsm
