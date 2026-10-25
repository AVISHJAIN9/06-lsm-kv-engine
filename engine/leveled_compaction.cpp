#include "leveled_compaction.hpp"
#include <cmath>
#include <algorithm>
#include <queue>
#include <iostream>

namespace lsm {

namespace {

struct HeapItem {
    Slice key;
    size_t iter_idx;

    bool operator>(const HeapItem& other) const {
        return key.compare(other.key) > 0;
    }
};

} // namespace

LeveledCompactor::LeveledCompactor(LeveledCompactionOptions options)
    : options_(options) {}

uint64_t LeveledCompactor::TargetBytesForLevel(int level) const {
    if (level <= 0) return 0;
    double target = options_.base_level_bytes;
    for (int i = 1; i < level; ++i) {
        target *= options_.level_multiplier;
    }
    return static_cast<uint64_t>(target);
}

void LeveledCompactor::FindOverlappingFiles(
    const std::string& smallest,
    const std::string& largest,
    const std::vector<FileMetaData>& next_level_files,
    std::vector<FileMetaData>* out_overlapping
) const {
    if (smallest.empty() || largest.empty()) return;

    for (const auto& file : next_level_files) {
        // Range check: file [f_small, f_large] overlaps [smallest, largest]
        if (Slice(file.smallest_key).compare(Slice(largest)) <= 0 &&
            Slice(file.largest_key).compare(Slice(smallest)) >= 0) {
            out_overlapping->push_back(file);
        }
    }
}

bool LeveledCompactor::PickCompaction(
    const std::vector<std::vector<FileMetaData>>& levels,
    LeveledCompactionJob* out_job
) const {
    if (!out_job || levels.empty()) return false;

    int best_level = -1;
    double best_score = 0.0;

    // 1. Evaluate Level 0 score
    if (!levels[0].empty()) {
        double l0_score = static_cast<double>(levels[0].size()) / options_.l0_file_num_compaction_trigger;
        if (l0_score > best_score) {
            best_score = l0_score;
            best_level = 0;
        }
    }

    // 2. Evaluate Levels 1..N scores
    for (size_t l = 1; l < levels.size(); ++l) {
        uint64_t level_bytes = 0;
        for (const auto& f : levels[l]) {
            level_bytes += f.file_size;
        }
        uint64_t target_bytes = TargetBytesForLevel(static_cast<int>(l));
        if (target_bytes > 0) {
            double score = static_cast<double>(level_bytes) / target_bytes;
            if (score > best_score) {
                best_score = score;
                best_level = static_cast<int>(l);
            }
        }
    }

    if (best_score < 1.0 || best_level < 0 || static_cast<size_t>(best_level) >= levels.size() - 1) {
        return false;
    }

    out_job->level = best_level;
    out_job->score = best_score;

    if (best_level == 0) {
        // Level 0 has overlapping files: take all L0 files
        out_job->inputs_level = levels[0];
    } else {
        // Pick first file from Level i
        out_job->inputs_level.push_back(levels[best_level][0]);
    }

    // Determine aggregate [smallest, largest] across selected inputs
    std::string smallest_key, largest_key;
    for (const auto& f : out_job->inputs_level) {
        if (smallest_key.empty() || Slice(f.smallest_key).compare(Slice(smallest_key)) < 0) {
            smallest_key = f.smallest_key;
        }
        if (largest_key.empty() || Slice(f.largest_key).compare(Slice(largest_key)) > 0) {
            largest_key = f.largest_key;
        }
    }

    // Find overlapping files in Level i+1
    const auto& next_level = levels[best_level + 1];
    FindOverlappingFiles(smallest_key, largest_key, next_level, &out_job->inputs_next_level);

    return true;
}

Status LeveledCompactor::ExecuteCompaction(
    const std::string& db_dir,
    uint64_t* next_file_number,
    LeveledCompactionJob* job,
    std::shared_ptr<BlockCache> block_cache
) {
    if (!job || !next_file_number) {
        return Status::InvalidArgument("Null pointer passed to ExecuteCompaction");
    }

    TableOptions table_opts;
    table_opts.block_size = 4096;
    table_opts.filter_policy = std::make_shared<BloomFilterPolicy>(10);

    // 1. Open all input readers
    std::vector<std::unique_ptr<TableReader>> readers;
    std::vector<std::unique_ptr<Iterator>> iterators;

    auto open_files = [&](const std::vector<FileMetaData>& files) -> Status {
        for (const auto& f : files) {
            std::unique_ptr<TableReader> r;
            Status s = TableReader::Open(table_opts, f.filename, f.file_size, block_cache, &r);
            if (!s.ok()) return s;
            iterators.push_back(r->NewIterator());
            readers.push_back(std::move(r));
        }
        return Status::OK();
    };

    Status s = open_files(job->inputs_level);
    if (!s.ok()) return s;
    s = open_files(job->inputs_next_level);
    if (!s.ok()) return s;

    // 2. Initialize iterators
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> heap;
    for (size_t i = 0; i < iterators.size(); ++i) {
        iterators[i]->SeekToFirst();
        if (iterators[i]->Valid()) {
            heap.push({iterators[i]->key(), i});
        }
    }

    std::unique_ptr<TableBuilder> builder;
    std::string current_out_filename;
    uint64_t current_out_file_num = 0;

    auto start_new_output_file = [&]() -> Status {
        current_out_file_num = (*next_file_number)++;
        char name_buf[128];
        std::snprintf(name_buf, sizeof(name_buf), "%s/%06llu.sst", db_dir.c_str(), static_cast<unsigned long long>(current_out_file_num));
        current_out_filename = name_buf;

        builder = std::make_unique<TableBuilder>(table_opts, current_out_filename);
        return builder->status();
    };

    s = start_new_output_file();
    if (!s.ok()) return s;

    std::string last_processed_key;
    bool has_last_key = false;

    // 3. Multi-way merge loop with deduplication
    while (!heap.empty()) {
        HeapItem top = heap.top();
        heap.pop();

        size_t iter_idx = top.iter_idx;
        Slice key = iterators[iter_idx]->key();
        Slice value = iterators[iter_idx]->value();

        // Key deduplication: if duplicate key, first one (newest L0/Li) takes precedence
        if (!has_last_key || key.compare(Slice(last_processed_key)) != 0) {
            builder->Add(key, value);
            last_processed_key = key.ToString();
            has_last_key = true;

            // Split into new output file if target size reached
            if (builder->FileSize() >= options_.target_file_size) {
                Status finish_s = builder->Finish();
                if (!finish_s.ok()) return finish_s;

                FileMetaData out_meta;
                out_meta.file_number = current_out_file_num;
                out_meta.filename = current_out_filename;
                out_meta.file_size = builder->FileSize();
                out_meta.smallest_key = builder->properties().smallest_key;
                out_meta.largest_key = builder->properties().largest_key;
                job->output_files.push_back(out_meta);

                s = start_new_output_file();
                if (!s.ok()) return s;
                has_last_key = false;
            }
        }

        iterators[iter_idx]->Next();
        if (iterators[iter_idx]->Valid()) {
            heap.push({iterators[iter_idx]->key(), iter_idx});
        }
    }

    // 4. Finish the final output file if it contains entries
    if (builder && builder->NumEntries() > 0) {
        Status finish_s = builder->Finish();
        if (!finish_s.ok()) return finish_s;

        FileMetaData out_meta;
        out_meta.file_number = current_out_file_num;
        out_meta.filename = current_out_filename;
        out_meta.file_size = builder->FileSize();
        out_meta.smallest_key = builder->properties().smallest_key;
        out_meta.largest_key = builder->properties().largest_key;
        job->output_files.push_back(out_meta);
    } else if (builder) {
        builder->Abandon();
    }

    return Status::OK();
}

} // namespace lsm
