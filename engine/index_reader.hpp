#pragma once

#include <memory>
#include "slice.hpp"
#include "status.hpp"
#include "format.hpp"
#include "block_reader.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// IndexReader: Decodes SSTable sparse index blocks and locates target data blocks
// -----------------------------------------------------------------------------
class IndexReader {
public:
    explicit IndexReader(Slice index_block_contents);
    ~IndexReader() = default;

    IndexReader(const IndexReader&) = delete;
    IndexReader& operator=(const IndexReader&) = delete;

    // Binary searches index block to locate the candidate data BlockHandle for target key
    // Returns true if candidate block is found, false if key exceeds largest index key
    bool FindBlockHandle(const Slice& user_key, BlockHandle* out_handle) const;

    // Returns an iterator traversing all (separator_key, BlockHandle) entries
    std::unique_ptr<Iterator> NewIterator() const;

    size_t size() const { return reader_.size(); }

private:
    BlockReader reader_;
};

} // namespace lsm
