#include "point_lookup.hpp"
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

PointLookupEngine::PointLookupEngine(
    std::shared_ptr<MemTableQueue> memtable_queue,
    std::vector<std::vector<FileMetaData>> levels,
    TableOptions table_options,
    std::shared_ptr<BlockCache> block_cache,
    std::shared_ptr<ReadAmplificationTracker> raf_tracker
) : memtable_queue_(std::move(memtable_queue)),
    levels_(std::move(levels)),
    table_options_(std::move(table_options)),
    block_cache_(std::move(block_cache)),
    raf_tracker_(std::move(raf_tracker)) {
    if (table_options_.block_size == 0) {
        table_options_.block_size = 4096;
    }
    if (!table_options_.filter_policy) {
        table_options_.filter_policy = std::make_shared<BloomFilterPolicy>(10);
    }
}

Status PointLookupEngine::Get(
    const Slice& user_key,
    SequenceNumber snapshot_seq,
    std::string* value,
    int* out_tier_hit
) {
    if (raf_tracker_) {
        raf_tracker_->RecordLogicalGet();
    }

    Status s;

    // -------------------------------------------------------------------------
    // Tier 1: Active MemTable Query
    // -------------------------------------------------------------------------
    if (memtable_queue_) {
        auto active = memtable_queue_->GetActive();
        if (active) {
            bool found = active->Get(user_key, snapshot_seq, value, &s);
            if (found) {
                if (raf_tracker_) {
                    raf_tracker_->RecordMemTableHit();
                }
                if (out_tier_hit) *out_tier_hit = 1;
                return s; // If tombstone (kTypeDeletion), s is Status::NotFound -> short-circuits!
            }
        }

        // ---------------------------------------------------------------------
        // Tier 2: Immutable MemTable Queue (Newest to Oldest)
        // ---------------------------------------------------------------------
        auto imm_queue = memtable_queue_->GetImmutableQueue();
        for (const auto& imm : imm_queue) {
            bool found = imm->Get(user_key, snapshot_seq, value, &s);
            if (found) {
                if (raf_tracker_) {
                    raf_tracker_->RecordMemTableHit();
                }
                if (out_tier_hit) *out_tier_hit = 2;
                return s; // Short-circuits immediately upon finding version or tombstone
            }
        }
    }

    // Helper lambda to query an SSTable file
    auto probe_sstable_file = [&](const FileMetaData& file_meta, int tier_id) -> Status {
        if (raf_tracker_) {
            raf_tracker_->RecordSSTableProbe();
        }

        std::unique_ptr<TableReader> reader;
        Status open_s = TableReader::Open(table_options_, file_meta.filename, file_meta.file_size, block_cache_, &reader);
        if (!open_s.ok()) {
            return open_s;
        }

        auto iter = reader->NewIterator();
        if (!iter) {
            return Status::Corruption("Failed to create table iterator for " + file_meta.filename);
        }

        // Seek to user_key
        iter->Seek(user_key);

        while (iter->Valid()) {
            Slice full_key = iter->key();
            if (full_key.size() >= 8) {
                InternalKey found_ikey;
                found_ikey.DecodeFrom(full_key);

                if (found_ikey.user_key().compare(user_key) == 0) {
                    if (found_ikey.sequence() <= snapshot_seq) {
                        if (found_ikey.type() == kTypeValue) {
                            if (value) {
                                *value = iter->value().ToString();
                            }
                            if (out_tier_hit) *out_tier_hit = tier_id;
                            return Status::OK();
                        } else if (found_ikey.type() == kTypeDeletion) {
                            if (out_tier_hit) *out_tier_hit = tier_id;
                            return Status::NotFound("Key deleted in SSTable " + file_meta.filename);
                        }
                    }
                } else {
                    // Passed beyond user_key
                    break;
                }
            } else {
                // Plain key format
                if (full_key.compare(user_key) == 0) {
                    if (value) {
                        *value = iter->value().ToString();
                    }
                    if (out_tier_hit) *out_tier_hit = tier_id;
                    return Status::OK();
                } else {
                    break;
                }
            }
            iter->Next();
        }

        return Status::NotFound();
    };

    // -------------------------------------------------------------------------
    // Tier 3: Level 0 SSTables (Newest to Oldest)
    // -------------------------------------------------------------------------
    if (!levels_.empty() && !levels_[0].empty()) {
        std::vector<FileMetaData> l0_files = levels_[0];
        std::sort(l0_files.begin(), l0_files.end(), [](const FileMetaData& a, const FileMetaData& b) {
            return a.file_number > b.file_number;
        });

        for (const auto& file : l0_files) {
            Slice f_small = ExtractUserKey(file.smallest_key);
            Slice f_large = ExtractUserKey(file.largest_key);

            if (!f_small.empty() && !f_large.empty()) {
                if (user_key.compare(f_small) < 0 || user_key.compare(f_large) > 0) {
                    continue; // Skip file outside key bounds
                }
            }

            Status probe_s = probe_sstable_file(file, 3);
            if (probe_s.ok()) {
                return Status::OK();
            }
            if (probe_s.IsNotFound() && probe_s.ToString().find("deleted") != std::string::npos) {
                return probe_s;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Tier 4: Levels 1..Lmax (Binary search sorted non-overlapping files per level)
    // -------------------------------------------------------------------------
    for (size_t l = 1; l < levels_.size(); ++l) {
        const auto& level_files = levels_[l];
        if (level_files.empty()) continue;

        int left = 0, right = static_cast<int>(level_files.size()) - 1;
        int candidate_idx = -1;

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
                candidate_idx = mid;
                break;
            }
        }

        if (candidate_idx >= 0) {
            Status probe_s = probe_sstable_file(level_files[candidate_idx], 3 + static_cast<int>(l));
            if (probe_s.ok()) {
                return Status::OK();
            }
            if (probe_s.IsNotFound() && probe_s.ToString().find("deleted") != std::string::npos) {
                return probe_s;
            }
        }
    }

    return Status::NotFound("Key not found in any storage tier");
}

} // namespace lsm
