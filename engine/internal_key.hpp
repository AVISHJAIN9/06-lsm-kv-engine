#pragma once

#include <string>
#include <cstdint>
#include <iostream>
#include "slice.hpp"
#include "types.hpp"

namespace lsm {

// Extract helper functions with boundary validation
inline Slice ExtractUserKeyFromInternalKey(const Slice& ikey) {
    if (ikey.size() < 8) {
        return ikey;
    }
    return Slice(ikey.data(), ikey.size() - 8);
}

inline uint64_t ExtractTagFromInternalKey(const Slice& ikey) {
    if (ikey.size() < 8) {
        return 0;
    }
    uint64_t tag = 0;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(ikey.data() + ikey.size() - 8);
    for (int i = 0; i < 8; ++i) {
        tag |= (static_cast<uint64_t>(ptr[i]) << (i * 8));
    }
    return tag;
}

inline bool ParseInternalKey(const Slice& ikey, ParsedInternalKey* result) {
    if (ikey.size() < 8) {
        return false;
    }
    uint64_t tag = ExtractTagFromInternalKey(ikey);
    result->user_key = Slice(ikey.data(), ikey.size() - 8);
    result->sequence = ExtractSequenceNumber(tag);
    result->type = ExtractValueType(tag);
    return (result->type <= kTypeValue);
}

// LookupKey helper: Helper for point lookups with snapshot sequence
class LookupKey {
public:
    LookupKey(const Slice& user_key, SequenceNumber sequence);
    ~LookupKey() = default;

    LookupKey(const LookupKey&) = default;
    LookupKey& operator=(const LookupKey&) = default;

    Slice memtable_key() const { return Slice(space_); }
    Slice internal_key() const { return Slice(kstart_, (space_.data() + space_.size()) - kstart_); }
    Slice user_key() const { return Slice(kstart_, (space_.data() + space_.size() - 8) - kstart_); }

private:
    std::string space_{};
    const char* kstart_{nullptr};
};

// Comparator class encapsulating internal key ordering rules
class InternalKeyComparatorImpl {
public:
    InternalKeyComparatorImpl() = default;

    const char* Name() const { return "lsm.InternalKeyComparator"; }

    int Compare(const Slice& a, const Slice& b) const {
        return InternalKeyComparator(a, b);
    }

    int Compare(const InternalKey& a, const InternalKey& b) const {
        return InternalKeyComparator(a.Encode(), b.Encode());
    }

    int Compare(const ParsedInternalKey& a, const ParsedInternalKey& b) const;
};

// Formats internal key for debugging / logging: "[user_key, seq, type]"
std::string DebugString(const Slice& internal_key);
std::string DebugString(const ParsedInternalKey& parsed);

} // namespace lsm
