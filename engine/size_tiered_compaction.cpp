#include "size_tiered_compaction.hpp"
#include <algorithm>
#include <queue>

namespace lsm {

namespace {

struct Bucket {
    std::vector<FileMetaData> files;
    double total_bytes{0.0};

    double Average() const {
        return files.empty() ? 0.0 : (total_bytes / files.size());
    }
};

struct HeapItem {
    Slice key;
    size_t iter_idx;

    bool operator>(const HeapItem& other) const {
        return key.compare(other.key) > 0;
    }
};

} // namespace

SizeTieredCompactor::SizeTieredCompactor(SizeTieredCompactionOptions options)
    : options_(options) {}

bool SizeTieredCompactor::PickCompaction(
    const std::vector<FileMetaData>& all_files,
    SizeTieredCompactionJob* out_job
) const {
    if (!out_job || all_files.size() < options_.min_threshold) {
        return false;
    }

    std::vector<FileMetaData> sorted_files = all_files;
    std::sort(sorted_files.begin(), sorted_files.end(), [](const FileMetaData& a, const FileMetaData& b) {
        return a.file_size < b.file_size;
    });

    std::vector<Bucket> buckets;

    for (const auto& file : sorted_files) {
        bool added = false;
        for (auto& bucket : buckets) {
            double avg = bucket.Average();
            if (file.file_size >= avg * options_.bucket_low &&
                file.file_size <= avg * options_.bucket_high) {
                bucket.files.push_back(file);
                bucket.total_bytes += file.file_size;
                added = true;
                break;
            }
        }

        if (!added) {
            Bucket new_bucket;
            new_bucket.files.push_back(file);
            new_bucket.total_bytes = file.file_size;
            buckets.push_back(std::move(new_bucket));
        }
    }

    for (const auto& bucket : buckets) {
        if (bucket.files.size() >= options_.min_threshold) {
            size_t count = std::min(bucket.files.size(), options_.max_threshold);
            out_job->input_files.assign(bucket.files.begin(), bucket.files.begin() + count);
            out_job->average_size = bucket.Average();
            return true;
        }
    }

    return false;
}

Status SizeTieredCompactor::ExecuteCompaction(
    const std::string& db_dir,
    uint64_t next_file_number,
    SizeTieredCompactionJob* job,
    std::shared_ptr<BlockCache> block_cache
) {
    if (!job || job->input_files.empty()) {
        return Status::InvalidArgument("No input files for size-tiered compaction");
    }

    TableOptions table_opts;
    table_opts.block_size = 4096;
    table_opts.filter_policy = std::make_shared<BloomFilterPolicy>(10);

    std::vector<std::unique_ptr<TableReader>> readers;
    std::vector<std::unique_ptr<Iterator>> iterators;

    for (const auto& f : job->input_files) {
        std::unique_ptr<TableReader> r;
        Status s = TableReader::Open(table_opts, f.filename, f.file_size, block_cache, &r);
        if (!s.ok()) return s;
        iterators.push_back(r->NewIterator());
        readers.push_back(std::move(r));
    }

    char out_name[128];
    std::snprintf(out_name, sizeof(out_name), "%s/%06llu.sst", db_dir.c_str(), static_cast<unsigned long long>(next_file_number));
    std::string out_filename = out_name;

    TableBuilder builder(table_opts, out_filename);
    if (!builder.status().ok()) {
        return builder.status();
    }

    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> heap;
    for (size_t i = 0; i < iterators.size(); ++i) {
        iterators[i]->SeekToFirst();
        if (iterators[i]->Valid()) {
            heap.push({iterators[i]->key(), i});
        }
    }

    std::string last_key;
    bool has_key = false;

    while (!heap.empty()) {
        HeapItem top = heap.top();
        heap.pop();

        size_t idx = top.iter_idx;
        Slice key = iterators[idx]->key();
        Slice value = iterators[idx]->value();

        if (!has_key || key.compare(Slice(last_key)) != 0) {
            builder.Add(key, value);
            last_key = key.ToString();
            has_key = true;
        }

        iterators[idx]->Next();
        if (iterators[idx]->Valid()) {
            heap.push({iterators[idx]->key(), idx});
        }
    }

    Status s = builder.Finish();
    if (!s.ok()) {
        return s;
    }

    job->output_file.file_number = next_file_number;
    job->output_file.filename = out_filename;
    job->output_file.file_size = builder.FileSize();
    job->output_file.smallest_key = builder.properties().smallest_key;
    job->output_file.largest_key = builder.properties().largest_key;

    return Status::OK();
}

} // namespace lsm
