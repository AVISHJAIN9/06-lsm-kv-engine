#pragma once

#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "leveled_compaction.hpp"
#include "memtable_queue.hpp"
#include "table_reader.hpp"
#include "block_cache.hpp"
#include "bloom_short_circuit.hpp"
#include "index_lookup.hpp"
#include "db_iterator.hpp"

namespace lsm {

struct LookupKey {
    Slice user_key;
    SequenceNumber snapshot_seq{kMaxSequenceNumber};

    LookupKey(const Slice& ukey, SequenceNumber seq)
        : user_key(ukey), snapshot_seq(seq) {}
};

// -----------------------------------------------------------------------------
// Version: Ref-counted snapshot representation of LSM levels and active SSTables
// -----------------------------------------------------------------------------
class Version {
public:
    explicit Version(std::vector<std::vector<FileMetaData>> files);
    ~Version() = default;

    void Ref() {
        refs_.fetch_add(1, std::memory_order_relaxed);
    }

    void Unref() {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    const std::vector<std::vector<FileMetaData>>& files() const { return files_; }
    const std::vector<FileMetaData>& LevelFiles(int level) const { return files_[level]; }
    size_t NumLevels() const { return files_.size(); }

    uint32_t RefCount() const { return refs_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint32_t> refs_{1};
    std::vector<std::vector<FileMetaData>> files_;
};

// -----------------------------------------------------------------------------
// VersionQueryRouter: Multi-Level Read Path Cascading & Query Engine
// -----------------------------------------------------------------------------
class VersionQueryRouter {
public:
    VersionQueryRouter(
        std::shared_ptr<MemTableQueue> memtable_queue,
        Version* version,
        TableOptions table_options = {},
        std::shared_ptr<ShardedLRUCache> block_cache = nullptr
    );

    // Executes cascading point query with Bloom filter short-circuit and pinpoint index lookup
    Status Get(
        const ReadOptionsExtended& options,
        const LookupKey& lkey,
        std::string* value,
        int* out_level_hit = nullptr
    );

    // Locates single overlapping candidate file in non-overlapping level 1..N via binary search
    static int BinarySearchLevel(
        const std::vector<FileMetaData>& level_files,
        const Slice& user_key
    );

    BloomShortCircuitStats GetBloomStats() const { return bloom_circuit_.GetStats(); }

private:
    std::shared_ptr<MemTableQueue> memtable_queue_;
    Version* version_{nullptr};
    TableOptions table_options_;
    std::shared_ptr<ShardedLRUCache> block_cache_;
    mutable BloomShortCircuit bloom_circuit_;
};

} // namespace lsm
