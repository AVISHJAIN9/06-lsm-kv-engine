#include "write_batch.hpp"
#include <cstring>

namespace lsm {

static constexpr size_t kHeaderSize = 12; // 8 bytes sequence + 4 bytes count

static void PutVarint32(std::string* dst, uint32_t v) {
    char buf[5];
    char* ptr = buf;
    while (v >= 0x80) {
        *(ptr++) = static_cast<char>((v & 0x7F) | 0x80);
        v >>= 7;
    }
    *(ptr++) = static_cast<char>(v);
    dst->append(buf, ptr - buf);
}

static bool GetVarint32(Slice* input, uint32_t* value) {
    uint32_t result = 0;
    int shift = 0;
    while (shift < 35) {
        if (input->empty()) return false;
        uint8_t byte = static_cast<uint8_t>((*input)[0]);
        input->remove_prefix(1);
        result |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

static void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
    PutVarint32(dst, static_cast<uint32_t>(value.size()));
    dst->append(value.data(), value.size());
}

static bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
    uint32_t len = 0;
    if (!GetVarint32(input, &len)) return false;
    if (input->size() < len) return false;
    *result = Slice(input->data(), len);
    input->remove_prefix(len);
    return true;
}

WriteBatch::WriteBatch() {
    Clear();
}

void WriteBatch::Clear() {
    rep_.clear();
    rep_.resize(kHeaderSize, 0);
}

uint32_t WriteBatch::Count() const {
    if (rep_.size() < kHeaderSize) return 0;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(rep_.data() + 8);
    return static_cast<uint32_t>(ptr[0]) |
          (static_cast<uint32_t>(ptr[1]) << 8) |
          (static_cast<uint32_t>(ptr[2]) << 16) |
          (static_cast<uint32_t>(ptr[3]) << 24);
}

void WriteBatch::SetCount(uint32_t n) {
    if (rep_.size() < kHeaderSize) rep_.resize(kHeaderSize, 0);
    rep_[8]  = static_cast<char>((n >> 0) & 0xFF);
    rep_[9]  = static_cast<char>((n >> 8) & 0xFF);
    rep_[10] = static_cast<char>((n >> 16) & 0xFF);
    rep_[11] = static_cast<char>((n >> 24) & 0xFF);
}

SequenceNumber WriteBatch::Sequence() const {
    if (rep_.size() < kHeaderSize) return 0;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(rep_.data());
    SequenceNumber seq = 0;
    for (int i = 0; i < 8; ++i) {
        seq |= (static_cast<SequenceNumber>(ptr[i]) << (i * 8));
    }
    return seq;
}

void WriteBatch::SetSequence(SequenceNumber seq) {
    if (rep_.size() < kHeaderSize) rep_.resize(kHeaderSize, 0);
    for (int i = 0; i < 8; ++i) {
        rep_[i] = static_cast<char>((seq >> (i * 8)) & 0xFF);
    }
}

void WriteBatch::Put(const Slice& key, const Slice& value) {
    SetCount(Count() + 1);
    rep_.push_back(static_cast<char>(ValueType::kTypeValue));
    PutLengthPrefixedSlice(&rep_, key);
    PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
    SetCount(Count() + 1);
    rep_.push_back(static_cast<char>(ValueType::kTypeDeletion));
    PutLengthPrefixedSlice(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& source) {
    SetCount(Count() + source.Count());
    if (source.rep_.size() > kHeaderSize) {
        rep_.append(source.rep_.data() + kHeaderSize, source.rep_.size() - kHeaderSize);
    }
}

Status WriteBatch::Iterate(Handler* handler) const {
    Slice input(rep_);
    if (input.size() < kHeaderSize) {
        return Status::Corruption("Malformed WriteBatch header");
    }

    input.remove_prefix(kHeaderSize);
    uint32_t found = 0;

    while (!input.empty()) {
        found++;
        char tag = input[0];
        input.remove_prefix(1);

        switch (static_cast<ValueType>(tag)) {
            case ValueType::kTypeValue: {
                Slice key, value;
                if (GetLengthPrefixedSlice(&input, &key) &&
                    GetLengthPrefixedSlice(&input, &value)) {
                    handler->Put(key, value);
                } else {
                    return Status::Corruption("Bad WriteBatch Put record");
                }
                break;
            }
            case ValueType::kTypeDeletion: {
                Slice key;
                if (GetLengthPrefixedSlice(&input, &key)) {
                    handler->Delete(key);
                } else {
                    return Status::Corruption("Bad WriteBatch Delete record");
                }
                break;
            }
            default:
                return Status::Corruption("Unknown ValueType in WriteBatch");
        }
    }

    if (found != Count()) {
        return Status::Corruption("WriteBatch count mismatch");
    }

    return Status::OK();
}

} // namespace lsm
