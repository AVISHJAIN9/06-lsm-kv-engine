#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "slice.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// BlockBuilder: Generates prefix-compressed SSTable data and index blocks
// Supports periodic restart points for fast O(log K) binary search random access
// -----------------------------------------------------------------------------
class BlockBuilder {
public:
    explicit BlockBuilder(int block_restart_interval = 16);
    ~BlockBuilder() = default;

    BlockBuilder(const BlockBuilder&) = delete;
    BlockBuilder& operator=(const BlockBuilder&) = delete;

    // Resets the builder state for reusing memory
    void Reset();

    // Appends a key/value pair. REQUIRES: key must be >= any previously added key
    void Add(const Slice& key, const Slice& value);

    // Finishes building the block and returns a Slice pointing to buffer_
    Slice Finish();

    // Returns approximate size of the block in bytes
    size_t CurrentSizeEstimate() const;

    bool Empty() const { return buffer_.empty(); }

private:
    int block_restart_interval_{16};
    std::string buffer_;
    std::vector<uint32_t> restarts_;
    int counter_{0};
    bool finished_{false};
    std::string last_key_;
};

} // namespace lsm
