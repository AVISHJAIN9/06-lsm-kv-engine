#include "block_builder.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>

namespace lsm {

namespace {

static char* EncodeVarint32(char* dst, uint32_t v) {
    uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
    static const int B = 128;
    if (v < (1 << 7)) {
        *(ptr++) = v;
    } else if (v < (1 << 14)) {
        *(ptr++) = v | B;
        *(ptr++) = v >> 7;
    } else if (v < (1 << 21)) {
        *(ptr++) = v | B;
        *(ptr++) = (v >> 7) | B;
        *(ptr++) = v >> 14;
    } else if (v < (1 << 28)) {
        *(ptr++) = v | B;
        *(ptr++) = (v >> 7) | B;
        *(ptr++) = (v >> 14) | B;
        *(ptr++) = v >> 21;
    } else {
        *(ptr++) = v | B;
        *(ptr++) = (v >> 7) | B;
        *(ptr++) = (v >> 14) | B;
        *(ptr++) = (v >> 21) | B;
        *(ptr++) = v >> 28;
    }
    return reinterpret_cast<char*>(ptr);
}

static inline void EncodeFixed32(char* buf, uint32_t value) {
    uint8_t* const buffer = reinterpret_cast<uint8_t*>(buf);
    buffer[0] = static_cast<uint8_t>(value);
    buffer[1] = static_cast<uint8_t>(value >> 8);
    buffer[2] = static_cast<uint8_t>(value >> 16);
    buffer[3] = static_cast<uint8_t>(value >> 24);
}

} // namespace

BlockBuilder::BlockBuilder(int block_restart_interval)
    : block_restart_interval_(block_restart_interval) {
    assert(block_restart_interval >= 1);
    restarts_.push_back(0); // First restart point is at offset 0
}

void BlockBuilder::Reset() {
    buffer_.clear();
    restarts_.clear();
    restarts_.push_back(0);
    counter_ = 0;
    finished_ = false;
    last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
    return (buffer_.size() +                        // Raw data buffer
            restarts_.size() * sizeof(uint32_t) +   // Restart array
            sizeof(uint32_t));                      // Restart array count
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
    Slice last_key_slice(last_key_);
    assert(!finished_);
    assert(counter_ <= block_restart_interval_);

    size_t shared = 0;
    if (counter_ < block_restart_interval_) {
        // Compute shared prefix
        const size_t min_len = std::min(last_key_slice.size(), key.size());
        while ((shared < min_len) && (last_key_slice[shared] == key[shared])) {
            shared++;
        }
    } else {
        // Restart compression run
        restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
        counter_ = 0;
    }
    const size_t non_shared = key.size() - shared;

    // Append: <shared><non_shared><value_len>
    char varint_buf[15];
    char* ptr = EncodeVarint32(varint_buf, static_cast<uint32_t>(shared));
    ptr = EncodeVarint32(ptr, static_cast<uint32_t>(non_shared));
    ptr = EncodeVarint32(ptr, static_cast<uint32_t>(value.size()));
    buffer_.append(varint_buf, ptr - varint_buf);

    // Append non-shared key bytes and value bytes
    buffer_.append(key.data() + shared, non_shared);
    buffer_.append(value.data(), value.size());

    // Update state
    last_key_.resize(shared);
    last_key_.append(key.data() + shared, non_shared);
    counter_++;
}

Slice BlockBuilder::Finish() {
    // Append restart offsets
    for (size_t i = 0; i < restarts_.size(); ++i) {
        char buf[sizeof(uint32_t)];
        EncodeFixed32(buf, restarts_[i]);
        buffer_.append(buf, sizeof(buf));
    }

    // Append restart count
    char count_buf[sizeof(uint32_t)];
    EncodeFixed32(count_buf, static_cast<uint32_t>(restarts_.size()));
    buffer_.append(count_buf, sizeof(count_buf));

    finished_ = true;
    return Slice(buffer_);
}

} // namespace lsm
