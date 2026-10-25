#include "memtable_iter.hpp"
#include <cstring>

namespace lsm {

namespace {

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

static inline uint64_t DecodeFixed64(const char* ptr) {
    const uint8_t* buffer = reinterpret_cast<const uint8_t*>(ptr);
    return (static_cast<uint64_t>(buffer[0])) |
           (static_cast<uint64_t>(buffer[1]) << 8) |
           (static_cast<uint64_t>(buffer[2]) << 16) |
           (static_cast<uint64_t>(buffer[3]) << 24) |
           (static_cast<uint64_t>(buffer[4]) << 32) |
           (static_cast<uint64_t>(buffer[5]) << 40) |
           (static_cast<uint64_t>(buffer[6]) << 48) |
           (static_cast<uint64_t>(buffer[7]) << 56);
}

static inline void EncodeFixed64(char* buf, uint64_t value) {
    uint8_t* const buffer = reinterpret_cast<uint8_t*>(buf);
    buffer[0] = static_cast<uint8_t>(value);
    buffer[1] = static_cast<uint8_t>(value >> 8);
    buffer[2] = static_cast<uint8_t>(value >> 16);
    buffer[3] = static_cast<uint8_t>(value >> 24);
    buffer[4] = static_cast<uint8_t>(value >> 32);
    buffer[5] = static_cast<uint8_t>(value >> 40);
    buffer[6] = static_cast<uint8_t>(value >> 48);
    buffer[7] = static_cast<uint8_t>(value >> 56);
}

static uint32_t VarintLength(uint64_t v) {
    int len = 1;
    while (v >= 128) {
        v >>= 7;
        len++;
    }
    return len;
}

} // namespace

MemTableIter::MemTableIter(MemTableCore::Table* table)
    : iter_(table) {}

bool MemTableIter::Valid() const {
    return iter_.Valid();
}

Slice MemTableIter::ExtractUserKey(const char* entry) {
    if (!entry) return Slice();
    uint32_t key_len = 0;
    const char* p = GetVarint32Ptr(entry, entry + 5, &key_len);
    if (!p) return Slice();
    return Slice(p, key_len);
}

uint64_t MemTableIter::ExtractTag(const char* entry) {
    if (!entry) return 0;
    uint32_t key_len = 0;
    const char* p = GetVarint32Ptr(entry, entry + 5, &key_len);
    if (!p) return 0;
    return DecodeFixed64(p + key_len);
}

Slice MemTableIter::ExtractValue(const char* entry) {
    if (!entry) return Slice();
    uint32_t key_len = 0;
    const char* p = GetVarint32Ptr(entry, entry + 5, &key_len);
    if (!p) return Slice();
    const char* val_header = p + key_len + 8;
    uint32_t val_len = 0;
    const char* val_ptr = GetVarint32Ptr(val_header, val_header + 5, &val_len);
    if (!val_ptr) return Slice();
    return Slice(val_ptr, val_len);
}

void MemTableIter::Seek(const Slice& target) {
    size_t key_len = target.size();
    size_t lookup_len = VarintLength(key_len) + key_len + 8;
    tmp_key_buf_.resize(lookup_len);

    char* dst = tmp_key_buf_.data();
    char* p = EncodeVarint32(dst, static_cast<uint32_t>(key_len));
    std::memcpy(p, target.data(), key_len);
    p += key_len;
    EncodeFixed64(p, PackSequenceAndType(kMaxSequenceNumber, kTypeValue));

    iter_.Seek(tmp_key_buf_.data());
}

void MemTableIter::SeekToFirst() {
    iter_.SeekToFirst();
}

void MemTableIter::SeekToLast() {
    iter_.SeekToLast();
}

void MemTableIter::Next() {
    iter_.Next();
}

void MemTableIter::Prev() {
    iter_.Prev();
}

Slice MemTableIter::key() const {
    assert(Valid());
    return ExtractUserKey(iter_.key());
}

Slice MemTableIter::value() const {
    assert(Valid());
    return ExtractValue(iter_.key());
}

Slice MemTableIter::user_key() const {
    assert(Valid());
    return ExtractUserKey(iter_.key());
}

SequenceNumber MemTableIter::sequence() const {
    assert(Valid());
    uint64_t tag = ExtractTag(iter_.key());
    return ExtractSequenceNumber(tag);
}

ValueType MemTableIter::type() const {
    assert(Valid());
    uint64_t tag = ExtractTag(iter_.key());
    return ExtractValueType(tag);
}

} // namespace lsm
