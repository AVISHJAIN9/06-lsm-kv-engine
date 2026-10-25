#include "table_builder.hpp"
#include "sstable_version.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace lsm {

namespace fs = std::filesystem;

TableBuilder::TableBuilder(const TableOptions& options, const std::string& filename)
    : options_(options),
      filename_(filename),
      tmp_filename_(filename + ".tmp"),
      data_block_builder_(options.block_restart_interval),
      index_builder_(1) {
    if (options_.filter_policy) {
        filter_builder_ = std::make_unique<FilterBlockBuilder>(options_.filter_policy.get(), 2048);
    }

    fd_ = open(tmp_filename_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        status_ = Status::IOError("Failed to open temporary SSTable file: " + tmp_filename_);
    }
}

TableBuilder::~TableBuilder() {
    if (!closed_ && fd_ >= 0) {
        Abandon();
    }
}

void TableBuilder::WriteBlock(const Slice& block_contents, BlockHandle* handle) {
    std::string block_buf(block_contents.data(), block_contents.size());
    BlockTrailerEnvelope::PackTrailer(&block_buf, options_.compression_type);

    handle->set_offset(offset_);
    handle->set_size(block_contents.size());

    ssize_t written = write(fd_, block_buf.data(), block_buf.size());
    if (written < 0 || static_cast<size_t>(written) != block_buf.size()) {
        status_ = Status::IOError("Failed to write SSTable block");
        return;
    }

    offset_ += block_buf.size();
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
    if (!status_.ok()) return;

    if (num_entries_ == 0) {
        properties_.smallest_key = key.ToString();
    }
    properties_.largest_key = key.ToString();

    if (pending_index_entry_) {
        index_builder_.AddIndexEntry(&last_key_of_flushed_block_, &key, pending_handle_);
        pending_index_entry_ = false;
    }

    if (filter_builder_) {
        filter_builder_->AddKey(key);
    }

    data_block_builder_.Add(key, value);
    last_key_ = key.ToString();
    num_entries_++;
    properties_.num_entries = num_entries_;
    properties_.raw_key_size += key.size();
    properties_.raw_value_size += value.size();

    if (data_block_builder_.CurrentSizeEstimate() >= options_.block_size) {
        Flush();
    }
}

void TableBuilder::Flush() {
    if (!status_.ok()) return;
    if (data_block_builder_.Empty()) return;

    Slice raw_data = data_block_builder_.Finish();
    WriteBlock(raw_data, &pending_handle_);

    last_key_of_flushed_block_ = last_key_;
    pending_index_entry_ = true;

    properties_.data_size += (raw_data.size() + BlockTrailerEnvelope::kTrailerSize);
    properties_.num_data_blocks++;

    data_block_builder_.Reset();
    if (filter_builder_) {
        filter_builder_->StartBlock(offset_);
    }
}

Status TableBuilder::Finish() {
    if (!status_.ok()) return status_;
    if (closed_) return Status::Corruption("TableBuilder is already closed");

    // 1. Flush any pending data block
    Flush();

    if (pending_index_entry_) {
        index_builder_.AddIndexEntry(&last_key_of_flushed_block_, nullptr, pending_handle_);
        pending_index_entry_ = false;
    }

    // 2. Write Filter Block
    BlockHandle filter_block_handle(0, 0);
    if (filter_builder_) {
        Slice filter_data = filter_builder_->Finish();
        WriteBlock(filter_data, &filter_block_handle);
        properties_.filter_size = filter_data.size() + BlockTrailerEnvelope::kTrailerSize;
    }

    // 3. Write Properties Block
    BlockHandle props_block_handle(0, 0);
    properties_.format_version = 2;
    properties_.feature_flags = TableFeatureFlags::kFlagBlockBloom;
    std::string props_encoded;
    properties_.EncodeTo(&props_encoded);
    WriteBlock(Slice(props_encoded), &props_block_handle);

    // 4. Write MetaIndex Block
    BlockBuilder meta_builder(1);
    if (filter_builder_) {
        std::string filter_handle_enc;
        filter_block_handle.EncodeTo(&filter_handle_enc);
        meta_builder.Add(Slice("filter." + std::string(options_.filter_policy->Name())), Slice(filter_handle_enc));
    }
    std::string props_handle_enc;
    props_block_handle.EncodeTo(&props_handle_enc);
    meta_builder.Add(Slice(kTablePropertiesKey), Slice(props_handle_enc));

    Slice meta_data = meta_builder.Finish();
    BlockHandle meta_block_handle;
    WriteBlock(meta_data, &meta_block_handle);

    // 5. Write Index Block
    Slice index_data = index_builder_.Finish();
    BlockHandle index_block_handle;
    WriteBlock(index_data, &index_block_handle);
    properties_.index_size = index_data.size() + BlockTrailerEnvelope::kTrailerSize;

    // 6. Write 48-byte Footer
    Footer footer;
    footer.set_metaindex_handle(meta_block_handle);
    footer.set_index_handle(index_block_handle);

    std::string footer_buf;
    footer.EncodeTo(&footer_buf);
    ssize_t written = write(fd_, footer_buf.data(), footer_buf.size());
    if (written < 0 || static_cast<size_t>(written) != footer_buf.size()) {
        Abandon();
        return Status::IOError("Failed to write SSTable footer");
    }
    offset_ += footer_buf.size();

    // 7. Flush and sync to disk
    fsync(fd_);
    close(fd_);
    fd_ = -1;

    // 8. Atomic Rename
    std::error_code ec;
    fs::rename(tmp_filename_, filename_, ec);
    if (ec) {
        Abandon();
        return Status::IOError("Failed to rename temporary SSTable file to destination: " + ec.message());
    }

    closed_ = true;
    return Status::OK();
}

void TableBuilder::Abandon() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    std::error_code ec;
    fs::remove(tmp_filename_, ec);
    closed_ = true;
}

} // namespace lsm
