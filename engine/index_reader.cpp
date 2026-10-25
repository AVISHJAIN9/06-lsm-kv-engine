#include "index_reader.hpp"

namespace lsm {

IndexReader::IndexReader(Slice index_block_contents)
    : reader_(index_block_contents) {}

bool IndexReader::FindBlockHandle(const Slice& user_key, BlockHandle* out_handle) const {
    if (!out_handle) return false;

    auto iter = reader_.NewIterator();
    if (!iter) return false;

    iter->Seek(user_key);
    if (!iter->Valid()) {
        return false;
    }

    Slice handle_slice = iter->value();
    Status s = out_handle->DecodeFrom(&handle_slice);
    return s.ok();
}

std::unique_ptr<Iterator> IndexReader::NewIterator() const {
    return reader_.NewIterator();
}

} // namespace lsm
