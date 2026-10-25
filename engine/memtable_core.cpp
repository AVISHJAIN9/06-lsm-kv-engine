#include "memtable_core.hpp"
#include "memtable_iter.hpp"
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

static uint32_t VarintLength(uint64_t v) {
    int len = 1;
    while (v >= 128) {
        v >>= 7;
        len++;
    }
    return len;
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

} // namespace

Slice MemTableCore::GetLengthPrefixedSlice(const char* data) {
    uint32_t len = 0;
    const char* p = GetVarint32Ptr(data, data + 5, &len);
    if (!p) return Slice();
    return Slice(p, len);
}

int MemTableCore::KeyComparator::operator()(const char* aptr, const char* bptr) const {
    if (aptr == nullptr && bptr == nullptr) return 0;
    if (aptr == nullptr) return -1;
    if (bptr == nullptr) return 1;

    Slice a = MemTableCore::GetLengthPrefixedSlice(aptr);
    Slice b = MemTableCore::GetLengthPrefixedSlice(bptr);
    int r = a.compare(b);
    if (r != 0) {
        return r;
    }

    const uint64_t a_tag = DecodeFixed64(a.data() + a.size());
    const uint64_t b_tag = DecodeFixed64(b.data() + b.size());
    if (a_tag > b_tag) {
        return -1; // Higher sequence numbers sort before lower sequence numbers
    } else if (a_tag < b_tag) {
        return 1;
    }
    return 0;
}

MemTableCore::MemTableCore()
    : table_(comparator_, &arena_) {}

const char* MemTableCore::EncodeEntry(
    Arena* arena,
    SequenceNumber seq,
    ValueType type,
    const Slice& key,
    const Slice& value
) {
    size_t key_size = key.size();
    size_t val_size = value.size();
    size_t encoded_len = VarintLength(key_size) + key_size + 8 + VarintLength(val_size) + val_size;

    char* buf = arena->AllocateAligned(encoded_len);
    char* p = EncodeVarint32(buf, static_cast<uint32_t>(key_size));
    std::memcpy(p, key.data(), key_size);
    p += key_size;

    uint64_t tag = PackSequenceAndType(seq, type);
    EncodeFixed64(p, tag);
    p += 8;

    p = EncodeVarint32(p, static_cast<uint32_t>(val_size));
    std::memcpy(p, value.data(), val_size);
    return buf;
}

void MemTableCore::Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
    const char* entry = EncodeEntry(&arena_, seq, type, key, value);
    table_.Insert(entry);
    num_entries_.fetch_add(1, std::memory_order_relaxed);
}

bool MemTableCore::Get(const Slice& user_key, SequenceNumber snapshot_seq, std::string* value, Status* s) {
    // Construct lookup key buffer on stack: [key_len varint] [key_bytes] [tag with snapshot_seq]
    size_t key_len = user_key.size();
    size_t lookup_len = VarintLength(key_len) + key_len + 8;
    char lookup_buf[512];
    char* dst = lookup_buf;
    if (lookup_len > sizeof(lookup_buf)) {
        dst = new char[lookup_len];
    }

    char* p = EncodeVarint32(dst, static_cast<uint32_t>(key_len));
    std::memcpy(p, user_key.data(), key_len);
    p += key_len;
    EncodeFixed64(p, PackSequenceAndType(snapshot_seq, kTypeValue));

    Table::Iterator iter(&table_);
    iter.Seek(dst);

    bool found = false;
    if (iter.Valid()) {
        const char* entry = iter.key();
        uint32_t entry_key_length = 0;
        const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &entry_key_length);
        if (key_ptr && (entry_key_length == user_key.size()) && (std::memcmp(key_ptr, user_key.data(), key_len) == 0)) {
            uint64_t tag = DecodeFixed64(key_ptr + entry_key_length);
            SequenceNumber seq = ExtractSequenceNumber(tag);
            ValueType type = ExtractValueType(tag);

            if (seq <= snapshot_seq) {
                if (type == kTypeValue) {
                    Slice val = GetLengthPrefixedSlice(key_ptr + entry_key_length + 8);
                    if (value) value->assign(val.data(), val.size());
                    if (s) *s = Status::OK();
                    found = true;
                } else if (type == kTypeDeletion) {
                    if (s) *s = Status::NotFound("Key was deleted");
                    found = true;
                }
            }
        }
    }

    if (dst != lookup_buf) {
        delete[] dst;
    }

    if (!found) {
        if (s) *s = Status::NotFound("Key not found in MemTable");
    }

    return found;
}

std::unique_ptr<Iterator> MemTableCore::NewIterator() {
    return std::make_unique<MemTableIter>(&table_);
}

} // namespace lsm
