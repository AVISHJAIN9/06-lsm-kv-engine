#include "compaction_executor.hpp"
#include <queue>
#include <algorithm>
#include <iostream>

namespace lsm {

namespace {

struct HeapElement {
    Slice key;
    size_t iter_idx;

    bool operator>(const HeapElement& other) const {
        int cmp = InternalKeyComparator(key, other.key);
        if (cmp != 0) {
            return cmp > 0;
        }
        return iter_idx > other.iter_idx;
    }
};

} // namespace

CompactionExecutor::CompactionExecutor(
    CompactionExecutorOptions options,
    std::shared_ptr<WriteAmplificationTracker> waf_tracker
) : options_(options), waf_tracker_(std::move(waf_tracker)) {
    if (options_.table_options.block_size == 0) {
        options_.table_options.block_size = 4096;
    }
    if (!options_.table_options.filter_policy) {
        options_.table_options.filter_policy = std::make_shared<BloomFilterPolicy>(10);
    }
}

Status CompactionExecutor::Execute(
    const std::string& db_dir,
    uint64_t* next_file_number,
    LeveledCompactionJob* job,
    std::shared_ptr<BlockCache> block_cache,
    CancellationToken* cancel_token
) {
    if (!job || !next_file_number) {
        return Status::InvalidArgument("Null argument supplied to CompactionExecutor::Execute");
    }

    CompactionTempFileTracker temp_tracker;

    // 1. Open all input readers and initialize iterators
    std::vector<std::unique_ptr<TableReader>> readers;
    std::vector<std::unique_ptr<Iterator>> iterators;

    auto open_files = [&](const std::vector<FileMetaData>& files) -> Status {
        for (const auto& f : files) {
            if (cancel_token && cancel_token->IsCancelled()) {
                temp_tracker.Cleanup();
                return Status::Aborted("Compaction cancelled before opening files");
            }
            std::unique_ptr<TableReader> r;
            Status s = TableReader::Open(options_.table_options, f.filename, f.file_size, block_cache, &r);
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

    if (iterators.empty()) {
        return Status::OK();
    }

    // 2. Initialize min-heap over active iterators
    std::priority_queue<HeapElement, std::vector<HeapElement>, std::greater<HeapElement>> heap;
    for (size_t i = 0; i < iterators.size(); ++i) {
        iterators[i]->SeekToFirst();
        if (iterators[i]->Valid()) {
            heap.push({iterators[i]->key(), i});
        }
    }

    std::unique_ptr<TableBuilder> builder;
    std::string current_out_filename;
    uint64_t current_out_file_num = 0;
    uint64_t last_file_size_checkpoint = 0;

    auto start_new_output_file = [&]() -> Status {
        current_out_file_num = (*next_file_number)++;
        char name_buf[128];
        std::snprintf(name_buf, sizeof(name_buf), "%s/%06llu.sst",
                      db_dir.c_str(), static_cast<unsigned long long>(current_out_file_num));
        current_out_filename = name_buf;
        temp_tracker.RegisterFile(current_out_filename);

        builder = std::make_unique<TableBuilder>(options_.table_options, current_out_filename);
        last_file_size_checkpoint = 0;
        return builder->status();
    };

    s = start_new_output_file();
    if (!s.ok()) return s;

    std::string current_user_key;
    bool has_current_user_key = false;

    // 3. Multi-way min-heap merge loop
    while (!heap.empty()) {
        // Cooperative cancellation check
        if (cancel_token && cancel_token->IsCancelled()) {
            if (builder) {
                builder->Abandon();
            }
            temp_tracker.Cleanup();
            job->output_files.clear();
            return Status::Aborted("Compaction cancelled during execution");
        }

        HeapElement top = heap.top();
        heap.pop();

        size_t iter_idx = top.iter_idx;
        Slice full_key = iterators[iter_idx]->key();
        Slice value = iterators[iter_idx]->value();

        Slice ukey;
        SequenceNumber seq = 0;
        ValueType vtype = kTypeValue;

        if (full_key.size() >= 8) {
            InternalKey ikey;
            ikey.DecodeFrom(full_key);
            ukey = ikey.user_key();
            seq = ikey.sequence();
            vtype = ikey.type();
        } else {
            ukey = full_key;
            seq = 0;
            vtype = (value.empty() ? kTypeDeletion : kTypeValue);
        }

        bool is_first_occurrence = (!has_current_user_key || ukey.compare(Slice(current_user_key)) != 0);

        if (is_first_occurrence) {
            current_user_key = ukey.ToString();
            has_current_user_key = true;

            // Check if tombstone should be purged
            bool drop_tombstone = (vtype == kTypeDeletion && options_.is_bottom_level);

            if (!drop_tombstone) {
                builder->Add(full_key, value);

                // Rate limiting check
                if (options_.rate_limiter && builder->FileSize() > last_file_size_checkpoint) {
                    uint64_t diff = builder->FileSize() - last_file_size_checkpoint;
                    options_.rate_limiter->Request(diff);
                    last_file_size_checkpoint = builder->FileSize();
                }

                // Check if current SSTable has reached target size for splitting
                if (builder->FileSize() >= options_.target_file_size) {
                    Status finish_s = builder->Finish();
                    if (!finish_s.ok()) {
                        temp_tracker.Cleanup();
                        return finish_s;
                    }

                    FileMetaData out_meta;
                    out_meta.file_number = current_out_file_num;
                    out_meta.filename = current_out_filename;
                    out_meta.file_size = builder->FileSize();
                    out_meta.smallest_key = builder->properties().smallest_key;
                    out_meta.largest_key = builder->properties().largest_key;
                    job->output_files.push_back(out_meta);

                    if (waf_tracker_) {
                        waf_tracker_->RecordCompactionWrite(out_meta.file_size);
                    }

                    s = start_new_output_file();
                    if (!s.ok()) {
                        temp_tracker.Cleanup();
                        return s;
                    }
                }
            }
        } else {
            // Key is shadowed by a newer version of the same user_key
            // If snapshot retention is enabled and seq > oldest_snapshot_seq, keep it.
            if (options_.oldest_snapshot_seq > 0 && seq > options_.oldest_snapshot_seq) {
                builder->Add(full_key, value);

                if (options_.rate_limiter && builder->FileSize() > last_file_size_checkpoint) {
                    uint64_t diff = builder->FileSize() - last_file_size_checkpoint;
                    options_.rate_limiter->Request(diff);
                    last_file_size_checkpoint = builder->FileSize();
                }

                if (builder->FileSize() >= options_.target_file_size) {
                    Status finish_s = builder->Finish();
                    if (!finish_s.ok()) {
                        temp_tracker.Cleanup();
                        return finish_s;
                    }

                    FileMetaData out_meta;
                    out_meta.file_number = current_out_file_num;
                    out_meta.filename = current_out_filename;
                    out_meta.file_size = builder->FileSize();
                    out_meta.smallest_key = builder->properties().smallest_key;
                    out_meta.largest_key = builder->properties().largest_key;
                    job->output_files.push_back(out_meta);

                    if (waf_tracker_) {
                        waf_tracker_->RecordCompactionWrite(out_meta.file_size);
                    }

                    s = start_new_output_file();
                    if (!s.ok()) {
                        temp_tracker.Cleanup();
                        return s;
                    }
                }
            }
            // Otherwise, older shadowed version is discarded.
        }

        // Advance the iterator that yielded the current top element
        iterators[iter_idx]->Next();
        if (iterators[iter_idx]->Valid()) {
            heap.push({iterators[iter_idx]->key(), iter_idx});
        }
    }

    // 4. Finalize the last output file if non-empty
    if (builder && builder->NumEntries() > 0) {
        Status finish_s = builder->Finish();
        if (!finish_s.ok()) {
            temp_tracker.Cleanup();
            return finish_s;
        }

        FileMetaData out_meta;
        out_meta.file_number = current_out_file_num;
        out_meta.filename = current_out_filename;
        out_meta.file_size = builder->FileSize();
        out_meta.smallest_key = builder->properties().smallest_key;
        out_meta.largest_key = builder->properties().largest_key;
        job->output_files.push_back(out_meta);

        if (waf_tracker_) {
            waf_tracker_->RecordCompactionWrite(out_meta.file_size);
        }
    } else if (builder) {
        builder->Abandon();
    }

    temp_tracker.CommitAll();
    return Status::OK();
}

} // namespace lsm
