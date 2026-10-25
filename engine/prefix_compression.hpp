#pragma once

#include <string>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// PrefixCompressionCodec: Shared prefix delta encoder and separator generator
// -----------------------------------------------------------------------------
class PrefixCompressionCodec {
public:
    // Computes the common prefix length between two keys
    static size_t SharedPrefixLength(const Slice& a, const Slice& b);

    // Encodes a key-value entry relative to prev_key:
    // [shared_len: varint32][unshared_len: varint32][value_len: varint32][unshared_key_bytes][value_bytes]
    static void EncodeEntry(
        const Slice& prev_key,
        const Slice& curr_key,
        const Slice& value,
        std::string* dst
    );

    // Decodes an entry given previous reconstructed key
    // Returns pointer past the decoded entry, or nullptr on corruption
    static const char* DecodeEntry(
        const char* ptr,
        const char* limit,
        std::string* inout_key,
        Slice* out_value
    );

    // Computes the shortest string strictly greater than *start and <= limit
    // Used for constructing minimal index separators
    static void FindShortestSeparator(std::string* start, const Slice& limit);

    // Computes the shortest string strictly greater than *key
    static void FindShortSuccessor(std::string* key);
};

} // namespace lsm
