#pragma once

#include <memory>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "iterator.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// BlockReader: Decodes prefix-compressed SSTable blocks with restart-point acceleration
// -----------------------------------------------------------------------------
class BlockReader {
public:
    explicit BlockReader(Slice contents);
    ~BlockReader() = default;

    BlockReader(const BlockReader&) = delete;
    BlockReader& operator=(const BlockReader&) = delete;

    size_t size() const { return size_; }

    // Returns a bidirectional iterator over all entries in the block
    std::unique_ptr<Iterator> NewIterator() const;

private:
    friend class BlockIterator;

    const char* data_{nullptr};
    size_t size_{0};
    uint32_t restart_offset_{0};
    uint32_t num_restarts_{0};
};

} // namespace lsm
