#pragma once

#include <cstdint>
#include <string>
#include "slice.hpp"
#include "status.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// BlockHandle: Identifies the position and size of a data, index, or filter block
// -----------------------------------------------------------------------------
class BlockHandle {
public:
    enum { kMaxEncodedLength = 10 + 10 }; // 2x Varint64

    BlockHandle();
    BlockHandle(uint64_t offset, uint64_t size);

    uint64_t offset() const { return offset_; }
    void set_offset(uint64_t offset) { offset_ = offset; }

    uint64_t size() const { return size_; }
    void set_size(uint64_t size) { size_ = size; }

    void EncodeTo(std::string* dst) const;
    Status DecodeFrom(Slice* input);

private:
    uint64_t offset_{0};
    uint64_t size_{0};
};

// -----------------------------------------------------------------------------
// Footer: Fixed 48-byte trailing structure in SSTable files
// Contains handles to the metaindex block and index block + 8-byte magic number
// -----------------------------------------------------------------------------
class Footer {
public:
    // Fixed table magic number: 0xdb4775248b80fb57ull
    static constexpr uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

    // Encoded length of a footer is fixed to 48 bytes
    // (2 * BlockHandle::kMaxEncodedLength = 40 bytes + 8 bytes Magic)
    static constexpr size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

    Footer() = default;

    const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
    void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

    const BlockHandle& index_handle() const { return index_handle_; }
    void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

    void EncodeTo(std::string* dst) const;
    Status DecodeFrom(Slice* input);

private:
    BlockHandle metaindex_handle_;
    BlockHandle index_handle_;
};

} // namespace lsm
