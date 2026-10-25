#include "version_query.hpp"
#include <algorithm>

namespace lsm {

namespace {

inline Slice ExtractUserKey(const std::string& key_str) {
    if (key_str.size() >= 8) {
        return Slice(key_str.data(), key_str.size() - 8);
    }
    return Slice(key_str);
}

} // namespace

Version::Version(std::vector<std::vector<FileMetaData>> files)
    : files_(std::move(files)) {}

VersionQueryRouter::VersionQueryRouter(
    std::shared_ptr<MemTableQueue> memtable_queue,
    Version* version,
    TableOptions table_options,
    std::shared_ptr<ShardedLRUCache> block_cache
) : memtable_queue_(std::move(memtable_queue)),
    version_(version),
    table_options_(std::move(table_options)),
    block_cache_(std::move(block_cache)) {
    if (table_options_.block_size == 0) {
        table_options_.block_size = 4096;
    }
    if (!table_options_.filter_policy) {
        table_options_.filter_policy = std::make_shared<BloomFilterPolicy>(10);
    }
}

int VersionQueryRouter::BinarySearchLevel(
    const std::vector<FileMetaData>& level_files,
    const Slice& user_key
) {
    if (level_files.empty()) return -1;

    int left = 0, right = static_cast<int>(level_files.size()) - 1;
    while (left <= right) {
        int mid = left + (right - left) / 2;
        const auto& file = level_files[mid];
        Slice f_small = ExtractUserKey(file.smallest_key);
        Slice f_large = ExtractUserKey(file.largest_key);

        if (user_key.compare(f_small) < 0) {
            right = mid - 1;
        } else if (user_key.compare(f_large) > 0) {
            left = mid + 1;
        } else {
            return mid;
        }
    }
    return -1;
}

Status VersionQueryRouter::Get(
    const ReadOptionsExtended& options,
    const LookupKey& lkey,
    std::string* value,
    int* out_level_hit
) {
    Status s;

    // -------------------------------------------------------------------------
    // Step 1: Active MemTable Query
    // -------------------------------------------------------------------------
    if (memtable_queue_) {
        auto active = memtable_queue_->GetActive();
        if (active) {
            bool found = active->Get(lkey.user_key, lkey.snapshot_seq, value, &s);
            if (found) {
                if (out_level_hit) *out_level_hit = 0;
                return s; // Short-circuits immediately (if tombstone, s is NotFound)
            }
        }

        // ---------------------------------------------------------------------
        // Step 2: Immutable MemTable Queue (Newest to Oldest)
        // ---------------------------------------------------------------------
        auto imm_queue = memtable_queue_->GetImmutableQueue();
        for (const auto& imm : imm_queue) {
            bool found = imm->Get(lkey.user_key, lkey.snapshot_seq, value, &s);
            if (found) {
                if (out_level_hit) *out_level_hit = 0;
                return s; // Short-circuits
            }
        }
    }

    if (!version_) {
        return Status::NotFound("No version loaded");
    }

    // Helper lambda to probe a candidate SSTable file
    auto probe_file = [&](const FileMetaData& file_meta, int level_idx) -> Status {
        Slice f_small = ExtractUserKey(file_meta.smallest_key);
        Slice f_large = ExtractUserKey(file_meta.largest_key);

        if (!f_small.empty() && !f_large.empty()) {
            if (lkey.user_key.compare(f_small) < 0 || lkey.user_key.compare(f_large) > 0) {
                return Status::NotFound("Key outside SSTable key bounds");
            }
        }

        std::unique_ptr<TableReader> reader;
        Status open_s = TableReader::Open(table_options_, file_meta.filename, file_meta.file_size, nullptr, &reader);
        if (!open_s.ok()) {
            return open_s;
        }

        auto iter = reader->NewIterator();
        if (!iter) {
            return Status::Corruption("Failed to create iterator for SSTable: " + file_meta.filename);
        }

        iter->Seek(lkey.user_key);

        while (iter->Valid()) {
            Slice full_key = iter->key();
            if (full_key.size() >= 8) {
                InternalKey found_ikey;
                found_ikey.DecodeFrom(full_key);

                if (found_ikey.user_key().compare(lkey.user_key) == 0) {
                    if (found_ikey.sequence() <= lkey.snapshot_seq) {
                        if (found_ikey.type() == kTypeValue) {
                            if (value) {
                                *value = iter->value().ToString();
                            }
                            if (out_level_hit) *out_level_hit = level_idx;
                            return Status::OK();
                        } else if (found_ikey.type() == kTypeDeletion) {
                            if (out_level_hit) *out_level_hit = level_idx;
                            return Status::NotFound("Key deleted in Level " + std::to_string(level_idx));
                        }
                    }
                } else {
                    break; // Passed target user key
                }
            } else {
                if (full_key.compare(lkey.user_key) == 0) {
                    if (value) {
                        *value = iter->value().ToString();
                    }
                    if (out_level_hit) *out_level_hit = level_idx;
                    return Status::OK();
                } else {
                    break;
                }
            }
            iter->Next();
        }

        return Status::NotFound("Key not in file data");
    };

    // -------------------------------------------------------------------------
    // Step 3: Level 0 SSTables (Newest to Oldest)
    // -------------------------------------------------------------------------
    if (version_->NumLevels() > 0 && !version_->LevelFiles(0).empty()) {
        std::vector<FileMetaData> l0_files = version_->LevelFiles(0);
        std::sort(l0_files.begin(), l0_files.end(), [](const FileMetaData& a, const FileMetaData& b) {
            return a.file_number > b.file_number;
        });

        for (const auto& f : l0_files) {
            Status probe_s = probe_file(f, 0);
            if (probe_s.ok()) {
                return Status::OK();
            }
            if (probe_s.IsNotFound() && probe_s.ToString().find("deleted") != std::string::npos) {
                return probe_s; // Tombstone short-circuits
            }
        }
    }

    // -------------------------------------------------------------------------
    // Step 4: Levels 1..Lmax (Binary Search Per Non-Overlapping Level)
    // -------------------------------------------------------------------------
    for (size_t l = 1; l < version_->NumLevels(); ++l) {
        const auto& level_files = version_->LevelFiles(static_cast<int>(l));
        if (level_files.empty()) continue;

        int candidate_idx = BinarySearchLevel(level_files, lkey.user_key);
        if (candidate_idx >= 0) {
            Status probe_s = probe_file(level_files[candidate_idx], static_cast<int>(l));
            if (probe_s.ok()) {
                return Status::OK();
            }
            if (probe_s.IsNotFound() && probe_s.ToString().find("deleted") != std::string::npos) {
                return probe_s; // Tombstone short-circuits
            }
        }
    }

    (void)options;
    return Status::NotFound("Key not found in any level");
}

} // namespace lsm
