#pragma once

#include <cstdint>
#include <string>
#include <cstring>
#include "slice.hpp"

namespace lsm {

using SequenceNumber = uint64_t;
static constexpr SequenceNumber kMaxSequenceNumber = ((0x1ULL << 56) - 1);

enum ValueType : uint8_t {
    kTypeDeletion = 0x0,
    kTypeValue = 0x1
};

struct WriteOptions {
    bool sync{false};
};

struct ReadOptions {
    bool verify_checksums{false};
    bool fill_cache{true};
    SequenceNumber snapshot{kMaxSequenceNumber};
};

inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t) {
    return (seq << 8) | static_cast<uint8_t>(t);
}

inline SequenceNumber ExtractSequenceNumber(uint64_t tag) {
    return tag >> 8;
}

inline ValueType ExtractValueType(uint64_t tag) {
    return static_cast<ValueType>(tag & 0xFF);
}

struct ParsedInternalKey {
    Slice user_key{};
    SequenceNumber sequence{0};
    ValueType type{kTypeValue};

    ParsedInternalKey() = default;
    ParsedInternalKey(Slice u, SequenceNumber seq, ValueType t)
        : user_key(u), sequence(seq), type(t) {}
};

class InternalKey {
public:
    InternalKey() = default;
    InternalKey(Slice user_key, SequenceNumber s, ValueType t) {
        rep_.assign(user_key.data(), user_key.size());
        uint64_t tag = PackSequenceAndType(s, t);
        for (int i = 0; i < 8; ++i) {
            rep_.push_back(static_cast<char>((tag >> (i * 8)) & 0xFF));
        }
    }

    void DecodeFrom(Slice s) {
        rep_.assign(s.data(), s.size());
    }

    Slice Encode() const { return Slice(rep_); }

    Slice user_key() const {
        if (rep_.size() < 8) return Slice();
        return Slice(rep_.data(), rep_.size() - 8);
    }

    uint64_t tag() const {
        if (rep_.size() < 8) return 0;
        uint64_t t = 0;
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(rep_.data() + rep_.size() - 8);
        for (int i = 0; i < 8; ++i) {
            t |= (static_cast<uint64_t>(ptr[i]) << (i * 8));
        }
        return t;
    }

    SequenceNumber sequence() const { return ExtractSequenceNumber(tag()); }
    ValueType type() const { return ExtractValueType(tag()); }

    void clear() { rep_.clear(); }

private:
    std::string rep_{};
};

// Internal key comparator:
// 1. Ascending by user_key
// 2. Descending by SequenceNumber (higher/newer sequence number comes first)
// 3. Descending by ValueType
inline int InternalKeyComparator(const Slice& a, const Slice& b) {
    Slice a_user = (a.size() >= 8) ? Slice(a.data(), a.size() - 8) : a;
    Slice b_user = (b.size() >= 8) ? Slice(b.data(), b.size() - 8) : b;

    int r = a_user.compare(b_user);
    if (r != 0) {
        return r;
    }

    if (a.size() < 8 || b.size() < 8) {
        if (a.size() < b.size()) return -1;
        if (a.size() > b.size()) return 1;
        return 0;
    }

    uint64_t a_tag = 0, b_tag = 0;
    const uint8_t* a_ptr = reinterpret_cast<const uint8_t*>(a.data() + a.size() - 8);
    const uint8_t* b_ptr = reinterpret_cast<const uint8_t*>(b.data() + b.size() - 8);
    for (int i = 0; i < 8; ++i) {
        a_tag |= (static_cast<uint64_t>(a_ptr[i]) << (i * 8));
        b_tag |= (static_cast<uint64_t>(b_ptr[i]) << (i * 8));
    }

    // Larger sequence number comes before smaller sequence number
    if (a_tag > b_tag) return -1;
    if (a_tag < b_tag) return 1;
    return 0;
}

} // namespace lsm
