#include "internal_key.hpp"
#include <sstream>
#include <iomanip>

namespace lsm {

LookupKey::LookupKey(const Slice& user_key, SequenceNumber sequence) {
    size_t usize = user_key.size();
    size_t needed = usize + 13; // varint32 (max 5) + usize + 8 byte tag
    space_.reserve(needed);

    // Encode length of internal key: (usize + 8)
    uint32_t ikey_len = static_cast<uint32_t>(usize + 8);
    char buf[5];
    char* ptr = buf;
    uint32_t v = ikey_len;
    while (v >= 128) {
        *(ptr++) = static_cast<char>((v & 127) | 128);
        v >>= 7;
    }
    *(ptr++) = static_cast<char>(v);

    space_.assign(buf, ptr - buf);
    size_t kstart_idx = space_.size();
    space_.append(user_key.data(), user_key.size());

    uint64_t tag = PackSequenceAndType(sequence, kTypeValue);
    for (int i = 0; i < 8; ++i) {
        space_.push_back(static_cast<char>((tag >> (i * 8)) & 0xFF));
    }
    kstart_ = space_.data() + kstart_idx;
}

int InternalKeyComparatorImpl::Compare(const ParsedInternalKey& a, const ParsedInternalKey& b) const {
    int r = a.user_key.compare(b.user_key);
    if (r != 0) {
        return r;
    }
    if (a.sequence > b.sequence) return -1;
    if (a.sequence < b.sequence) return 1;
    if (static_cast<uint8_t>(a.type) > static_cast<uint8_t>(b.type)) return -1;
    if (static_cast<uint8_t>(a.type) < static_cast<uint8_t>(b.type)) return 1;
    return 0;
}

std::string DebugString(const Slice& internal_key) {
    ParsedInternalKey parsed;
    if (!ParseInternalKey(internal_key, &parsed)) {
        return "(bad internal key)";
    }
    return DebugString(parsed);
}

std::string DebugString(const ParsedInternalKey& parsed) {
    std::ostringstream oss;
    oss << "['" << parsed.user_key.ToString() << "' @ seq=" << parsed.sequence << " (";
    if (parsed.type == kTypeValue) {
        oss << "Value";
    } else if (parsed.type == kTypeDeletion) {
        oss << "Deletion";
    } else {
        oss << "Type:" << static_cast<int>(parsed.type);
    }
    oss << ")]";
    return oss.str();
}

} // namespace lsm
