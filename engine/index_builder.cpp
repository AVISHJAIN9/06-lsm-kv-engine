#include "index_builder.hpp"
#include "prefix_compression.hpp"

namespace lsm {

IndexBuilder::IndexBuilder(int restart_interval)
    : block_builder_(restart_interval) {}

void IndexBuilder::Reset() {
    block_builder_.Reset();
    last_key_.clear();
    num_entries_ = 0;
}

void IndexBuilder::AddIndexEntry(
    std::string* last_key_in_current_block,
    const Slice* first_key_in_next_block,
    const BlockHandle& handle
) {
    if (first_key_in_next_block != nullptr) {
        PrefixCompressionCodec::FindShortestSeparator(last_key_in_current_block, *first_key_in_next_block);
    } else {
        PrefixCompressionCodec::FindShortSuccessor(last_key_in_current_block);
    }

    std::string handle_encoding;
    handle.EncodeTo(&handle_encoding);

    block_builder_.Add(Slice(*last_key_in_current_block), Slice(handle_encoding));
    last_key_ = *last_key_in_current_block;
    num_entries_++;
}

Slice IndexBuilder::Finish() {
    return block_builder_.Finish();
}

TwoLevelIndexBuilder::TwoLevelIndexBuilder(size_t max_partition_size, int restart_interval)
    : max_partition_size_(max_partition_size),
      restart_interval_(restart_interval),
      current_partition_builder_(restart_interval),
      top_level_builder_(1) {}

void TwoLevelIndexBuilder::FlushCurrentPartition(std::vector<std::string>* out_completed_partition_blocks) {
    if (current_partition_builder_.Empty()) return;

    Slice partition_slice = current_partition_builder_.Finish();
    std::string partition_buf(partition_slice.data(), partition_slice.size());

    uint64_t offset = simulated_partition_offset_;
    uint64_t size = partition_buf.size();
    simulated_partition_offset_ += size;

    if (out_completed_partition_blocks) {
        out_completed_partition_blocks->push_back(std::move(partition_buf));
    }

    BlockHandle partition_handle(offset, size);
    std::string handle_encoding;
    partition_handle.EncodeTo(&handle_encoding);

    std::string top_separator = last_partition_key_;
    PrefixCompressionCodec::FindShortSuccessor(&top_separator);
    top_level_builder_.Add(Slice(top_separator), Slice(handle_encoding));

    current_partition_builder_.Reset();
    total_partitions_++;
}

void TwoLevelIndexBuilder::AddIndexEntry(
    std::string* last_key_in_current_block,
    const Slice* first_key_in_next_block,
    const BlockHandle& handle,
    std::vector<std::string>* out_completed_partition_blocks
) {
    if (first_key_in_next_block != nullptr) {
        PrefixCompressionCodec::FindShortestSeparator(last_key_in_current_block, *first_key_in_next_block);
    } else {
        PrefixCompressionCodec::FindShortSuccessor(last_key_in_current_block);
    }

    std::string handle_encoding;
    handle.EncodeTo(&handle_encoding);

    current_partition_builder_.Add(Slice(*last_key_in_current_block), Slice(handle_encoding));
    last_partition_key_ = *last_key_in_current_block;

    if (current_partition_builder_.CurrentSizeEstimate() >= max_partition_size_) {
        FlushCurrentPartition(out_completed_partition_blocks);
    }
}

Slice TwoLevelIndexBuilder::Finish(std::vector<std::string>* out_completed_partition_blocks) {
    FlushCurrentPartition(out_completed_partition_blocks);
    return top_level_builder_.Finish();
}

} // namespace lsm
