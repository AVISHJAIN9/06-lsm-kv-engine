#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include "slice.hpp"
#include "status.hpp"
#include "format.hpp"
#include "index_reader.hpp"
#include "bloom_filter.hpp"
#include "table_properties.hpp"
#include "table_builder.hpp"
#include "block_reader.hpp"
#include "block_trailer_envelope.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// BlockCache: Thread-safe in-memory cache for decoded SSTable data blocks
// -----------------------------------------------------------------------------
class BlockCache {
public:
    explicit BlockCache(size_t capacity_bytes = 8 * 1024 * 1024);
    ~BlockCache() = default;

    std::shared_ptr<std::string> Get(const std::string& key);
    void Insert(const std::string& key, std::shared_ptr<std::string> value);

    size_t TotalHits() const { return hits_; }
    size_t TotalMisses() const { return misses_; }
    size_t capacity() const { return capacity_; }

private:
    mutable std::mutex mu_;
    size_t capacity_;
    std::unordered_map<std::string, std::shared_ptr<std::string>> map_;
    std::atomic<size_t> hits_{0};
    std::atomic<size_t> misses_{0};
};

// -----------------------------------------------------------------------------
// TableReader: High-throughput SSTable reader with BlockCache and Bloom filter
// -----------------------------------------------------------------------------
class TableReader {
public:
    static Status Open(
        const TableOptions& options,
        std::string filename,
        uint64_t file_size,
        std::shared_ptr<BlockCache> block_cache,
        std::unique_ptr<TableReader>* out_reader
    );

    ~TableReader();

    TableReader(const TableReader&) = delete;
    TableReader& operator=(const TableReader&) = delete;

    // Point lookup with Bloom filter probe, index search, and block cache acceleration
    Status Get(const Slice& user_key, std::string* value) const;

    // Returns a two-level iterator over all data entries in the SSTable
    std::unique_ptr<Iterator> NewIterator() const;
    std::unique_ptr<Iterator> NewIndexIterator() const { return index_reader_ ? index_reader_->NewIterator() : nullptr; }

    const TableProperties& properties() const { return properties_; }
    uint64_t FileSize() const { return file_size_; }
    const std::string& Filename() const { return filename_; }

    Status ReadRawBlock(const BlockHandle& handle, std::string* out_payload) const;

private:
    TableReader(TableOptions options, std::string filename, uint64_t file_size, int fd, std::shared_ptr<BlockCache> cache);

    Status Init();

    TableOptions options_;
    std::string filename_;
    uint64_t file_size_{0};
    int fd_{-1};
    std::shared_ptr<BlockCache> block_cache_;

    Footer footer_;
    TableProperties properties_;
    std::string filter_data_;
    std::unique_ptr<FilterBlockReader> filter_reader_;
    std::string index_data_;
    std::unique_ptr<IndexReader> index_reader_;
};

} // namespace lsm
