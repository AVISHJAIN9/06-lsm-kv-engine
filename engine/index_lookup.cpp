#include "index_lookup.hpp"

namespace lsm {

IndexPinpointLookup::IndexPinpointLookup(const IndexReader* index_reader)
    : index_reader_(index_reader) {}

bool IndexPinpointLookup::FindBlockHandle(const Slice& user_key, BlockHandle* out_handle) const {
    if (!index_reader_ || !out_handle) {
        return false;
    }
    return index_reader_->FindBlockHandle(user_key, out_handle);
}

Status IndexPinpointLookup::PinpointBlock(
    const Slice& user_key,
    const TableProperties& properties,
    BlockHandle* out_handle
) const {
    if (!out_handle) {
        return Status::InvalidArgument("Null pointer supplied for BlockHandle out parameter");
    }

    if (!properties.smallest_key.empty() && !properties.largest_key.empty()) {
        if (!properties.KeyInRange(user_key)) {
            return Status::NotFound("Key outside SSTable key range");
        }
    }

    if (!index_reader_) {
        return Status::Corruption("IndexReader is null");
    }

    if (!index_reader_->FindBlockHandle(user_key, out_handle)) {
        return Status::NotFound("Key not found in SSTable index");
    }

    return Status::OK();
}

} // namespace lsm
