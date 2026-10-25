#include "prefix_compression.hpp"
#include <algorithm>
#include <cstring>

namespace lsm {

namespace {

static char* EncodeVarint32(char* dst, uint32_t v) {
    uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
    static const int B = 128;
    while (v >= B) {
        *(ptr++) = (v & (B - 1)) | B;
        v >>= 7;
    }
    *(ptr++) = static_cast<uint8_t>(v);
    return reinterpret_cast<char*>(ptr);
}

static const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
    if (p >= limit) return nullptr;
    uint32_t result = 0;
    for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
        uint32_t byte = *(reinterpret_cast<const uint8_t*>(p));
        p++;
        if (byte & 128) {
            result |= ((byte & 127) << shift);
        } else {
            result |= (byte << shift);
            *value = result;
            return p;
        }
    }
    return nullptr;
}

} // namespace

size_t PrefixCompressionCodec::SharedPrefixLength(const Slice& a, const Slice& b) {
    const size_t min_len = std::min(a.size(), b.size());
    size_t shared = 0;
    while (shared < min_len && a[shared] == b[shared]) {
        shared++;
    }
    return shared;
}

void PrefixCompressionCodec::EncodeEntry(
    const Slice& prev_key,
    const Slice& curr_key,
    const Slice& value,
    std::string* dst
) {
    size_t shared = 0;
    if (!prev_key.empty()) {
        shared = SharedPrefixLength(prev_key, curr_key);
    }
    const size_t non_shared = curr_key.size() - shared;

    char varint_buf[15];
    char* ptr = EncodeVarint32(varint_buf, static_cast<uint32_t>(shared));
    ptr = EncodeVarint32(ptr, static_cast<uint32_t>(non_shared));
    ptr = EncodeVarint32(ptr, static_cast<uint32_t>(value.size()));
    dst->append(varint_buf, ptr - varint_buf);

    dst->append(curr_key.data() + shared, non_shared);
    dst->append(value.data(), value.size());
}

const char* PrefixCompressionCodec::DecodeEntry(
    const char* ptr,
    const char* limit,
    std::string* inout_key,
    Slice* out_value
) {
    if (ptr >= limit) return nullptr;

    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;

    ptr = GetVarint32Ptr(ptr, limit, &shared);
    if (!ptr) return nullptr;
    ptr = GetVarint32Ptr(ptr, limit, &non_shared);
    if (!ptr) return nullptr;
    ptr = GetVarint32Ptr(ptr, limit, &value_length);
    if (!ptr) return nullptr;

    if (static_cast<size_t>(limit - ptr) < (non_shared + value_length)) {
        return nullptr;
    }
    if (inout_key->size() < shared) {
        return nullptr;
    }

    inout_key->resize(shared);
    inout_key->append(ptr, non_shared);
    ptr += non_shared;

    if (out_value) {
        *out_value = Slice(ptr, value_length);
    }
    ptr += value_length;

    return ptr;
}

void PrefixCompressionCodec::FindShortestSeparator(std::string* start, const Slice& limit) {
    size_t min_length = std::min(start->size(), limit.size());
    size_t diff_index = 0;
    while (diff_index < min_length && (*start)[diff_index] == limit[diff_index]) {
        diff_index++;
    }

    if (diff_index < min_length) {
        uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);
        if (diff_byte < 0xff && diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
            (*start)[diff_index]++;
            start->resize(diff_index + 1);
        }
    }
}

void PrefixCompressionCodec::FindShortSuccessor(std::string* key) {
    for (size_t i = 0; i < key->size(); ++i) {
        const uint8_t byte = static_cast<uint8_t>((*key)[i]);
        if (byte < 0xff) {
            (*key)[i] = static_cast<char>(byte + 1);
            key->resize(i + 1);
            return;
        }
    }
}

} // namespace lsm
