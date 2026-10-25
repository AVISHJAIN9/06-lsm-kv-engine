#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "format.hpp"
#include "block_builder.hpp"
#include "index_builder.hpp"
#include "bloom_filter.hpp"
#include "table_properties.hpp"
#include "block_trailer_envelope.hpp"

namespace lsm {

struct TableOptions {
    size_t block_size{4096};
    int block_restart_interval{16};
    std::shared_ptr<BloomFilterPolicy> filter_policy{std::make_shared<BloomFilterPolicy>(10)};
    BlockTrailerEnvelope::CompressionType compression_type{BlockTrailerEnvelope::CompressionType::kNoCompression};
};

// -----------------------------------------------------------------------------
// TableBuilder: Streaming SSTable file builder with 2-phase atomic finalize
// -----------------------------------------------------------------------------
class TableBuilder {
public:
    TableBuilder(const TableOptions& options, const std::string& filename);
    ~TableBuilder();

    TableBuilder(const TableBuilder&) = delete;
    TableBuilder& operator=(const TableBuilder&) = delete;

    // Appends a sorted key-value pair. REQUIRES: key > last_key
    void Add(const Slice& key, const Slice& value);

    // Flushes the current data block if non-empty
    void Flush();

    // Finalizes the SSTable (writes filter, properties, index, footer) and renames from .tmp to .sst
    Status Finish();

    // Abandons the SSTable build on failure, removing the temporary file
    void Abandon();

    uint64_t FileSize() const { return offset_; }
    uint64_t NumEntries() const { return num_entries_; }
    Status status() const { return status_; }

    const TableProperties& properties() const { return properties_; }

private:
    void WriteBlock(const Slice& block_contents, BlockHandle* handle);

    TableOptions options_;
    std::string filename_;
    std::string tmp_filename_;
    int fd_{-1};
    uint64_t offset_{0};

    BlockBuilder data_block_builder_;
    IndexBuilder index_builder_;
    std::unique_ptr<FilterBlockBuilder> filter_builder_;
    TableProperties properties_;

    std::string last_key_;
    uint64_t num_entries_{0};
    bool closed_{false};
    Status status_{Status::OK()};

    bool pending_index_entry_{false};
    BlockHandle pending_handle_;
    std::string last_key_of_flushed_block_;
};

} // namespace lsm
