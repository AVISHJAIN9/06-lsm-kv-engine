#include "vector_read.hpp"
#include "index_lookup.hpp"
#include <algorithm>
#include <unordered_map>

namespace lsm {

namespace {

inline Slice ExtractUserKey(const std::string& key_str) {
    if (key_str.size() >= 8) {
        return Slice(key_str.data(), key_str.size() - 8);
    }
    return Slice(key_str);
}

struct QueryItem {
    size_t orig_idx{0};
    Slice key{};
    bool resolved{false};
};

} // namespace

VectorizedReader::VectorizedReader(
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

void VectorizedReader::MultiGet(
    const ReadOptionsExtended& options,
    const std::vector<Slice>& keys,
    std::vector<Status>* out_statuses,
    std::vector<std::string>* out_values,
    ReadTierStats* stats
) {
    if (!out_statuses || !out_values) return;

    out_statuses->assign(keys.size(), Status::NotFound("Key not found"));
    out_values->assign(keys.size(), "");

    if (keys.empty()) return;

    std::vector<QueryItem> items(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        items[i].orig_idx = i;
        items[i].key = keys[i];
        items[i].resolved = false;
    }

    // -------------------------------------------------------------------------
    // Phase 1: MemTable & ImmQueue Batch Lookup
    // -------------------------------------------------------------------------
    if (memtable_queue_) {
        for (auto& item : items) {
            std::string val;
            Status s;
            auto active = memtable_queue_->GetActive();
            if (active && active->Get(item.key, options.snapshot, &val, &s)) {
                (*out_statuses)[item.orig_idx] = s;
                (*out_values)[item.orig_idx] = val;
                item.resolved = true;
                if (stats) stats->memtable_hits++;
                continue;
            }

            auto imm_q = memtable_queue_->GetImmutableQueue();
            for (const auto& imm : imm_q) {
                if (imm->Get(item.key, options.snapshot, &val, &s)) {
                    (*out_statuses)[item.orig_idx] = s;
                    (*out_values)[item.orig_idx] = val;
                    item.resolved = true;
                    if (stats) stats->memtable_hits++;
                    break;
                }
            }
        }
    }

    if (!version_) return;

    // Helper lambda to query an SSTable for a batch of candidate keys
    auto probe_file_batch = [&](const FileMetaData& file_meta, std::vector<QueryItem*>& file_items, int level_idx) {
        if (file_items.empty()) return;
        if (stats) stats->files_probed++;

        std::unique_ptr<TableReader> reader;
        Status open_s = TableReader::Open(table_options_, file_meta.filename, file_meta.file_size, nullptr, &reader);
        if (!open_s.ok()) return;

        auto iter = reader->NewIterator();
        if (!iter) return;

        for (auto* item_ptr : file_items) {
            if (item_ptr->resolved) continue;

            iter->Seek(item_ptr->key);
            while (iter->Valid()) {
                Slice full_key = iter->key();
                if (full_key.size() >= 8) {
                    InternalKey found_ikey;
                    found_ikey.DecodeFrom(full_key);

                    if (found_ikey.user_key().compare(item_ptr->key) == 0) {
                        if (found_ikey.sequence() <= options.snapshot) {
                            if (found_ikey.type() == kTypeValue) {
                                (*out_values)[item_ptr->orig_idx] = iter->value().ToString();
                                (*out_statuses)[item_ptr->orig_idx] = Status::OK();
                                item_ptr->resolved = true;
                            } else if (found_ikey.type() == kTypeDeletion) {
                                (*out_statuses)[item_ptr->orig_idx] = Status::NotFound("Key deleted in level " + std::to_string(level_idx));
                                item_ptr->resolved = true;
                            }
                        }
                    } else {
                        break; // Passed target key
                    }
                } else {
                    if (full_key.compare(item_ptr->key) == 0) {
                        (*out_values)[item_ptr->orig_idx] = iter->value().ToString();
                        (*out_statuses)[item_ptr->orig_idx] = Status::OK();
                        item_ptr->resolved = true;
                    } else {
                        break;
                    }
                }
                iter->Next();
            }
        }
    };

    // -------------------------------------------------------------------------
    // Phase 2: Level 0 Files Batch Probe (Sorted newest to oldest)
    // -------------------------------------------------------------------------
    if (version_->NumLevels() > 0 && !version_->LevelFiles(0).empty()) {
        std::vector<FileMetaData> l0_files = version_->LevelFiles(0);
        std::sort(l0_files.begin(), l0_files.end(), [](const FileMetaData& a, const FileMetaData& b) {
            return a.file_number > b.file_number;
        });

        for (const auto& file : l0_files) {
            std::vector<QueryItem*> file_items;
            Slice f_small = ExtractUserKey(file.smallest_key);
            Slice f_large = ExtractUserKey(file.largest_key);

            for (auto& item : items) {
                if (item.resolved) continue;
                if (!f_small.empty() && !f_large.empty()) {
                    if (item.key.compare(f_small) < 0 || item.key.compare(f_large) > 0) {
                        continue;
                    }
                }
                file_items.push_back(&item);
            }

            probe_file_batch(file, file_items, 0);
        }
    }

    // -------------------------------------------------------------------------
    // Phase 3: Levels 1..Lmax Grouped Binary Search & Batch Resolution
    // -------------------------------------------------------------------------
    for (size_t l = 1; l < version_->NumLevels(); ++l) {
        const auto& level_files = version_->LevelFiles(static_cast<int>(l));
        if (level_files.empty()) continue;

        // Group unresolved items by candidate file index in level l
        std::unordered_map<int, std::vector<QueryItem*>> file_groups;

        for (auto& item : items) {
            if (item.resolved) continue;
            int file_idx = VersionQueryRouter::BinarySearchLevel(level_files, item.key);
            if (file_idx >= 0) {
                file_groups[file_idx].push_back(&item);
            }
        }

        for (auto& pair : file_groups) {
            const auto& file = level_files[pair.first];
            probe_file_batch(file, pair.second, static_cast<int>(l));
        }
    }
}

} // namespace lsm
