#include "cf_write_batch.hpp"

namespace lsm {

namespace {

void EncodeVarint32(std::string* dst, uint32_t v) {
    char buf[5];
    char* ptr = buf;
    while (v >= 128) {
        *(ptr++) = static_cast<char>((v & 127) | 128);
        v >>= 7;
    }
    *(ptr++) = static_cast<char>(v);
    dst->append(buf, ptr - buf);
}

bool DecodeVarint32(Slice* input, uint32_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    uint32_t result = 0;
    for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
        uint32_t byte = static_cast<uint8_t>(*p++);
        if (byte & 128) {
            result |= ((byte & 127) << shift);
        } else {
            result |= (byte << shift);
            *input = Slice(p, limit - p);
            *value = result;
            return true;
        }
    }
    return false;
}

bool DecodeLengthPrefixedSlice(Slice* input, Slice* result) {
    uint32_t len = 0;
    if (DecodeVarint32(input, &len) && input->size() >= len) {
        *result = Slice(input->data(), len);
        input->remove_prefix(len);
        return true;
    }
    return false;
}

} // anonymous namespace

CFWriteBatch::CFWriteBatch() {
    Clear();
}

void CFWriteBatch::Clear() {
    rep_.clear();
    rep_.resize(12, 0); // 4-byte count + 8-byte sequence
}

uint32_t CFWriteBatch::Count() const {
    if (rep_.size() < 12) return 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(rep_.data());
    uint32_t c = 0;
    for (int i = 0; i < 4; ++i) {
        c |= (static_cast<uint32_t>(p[i]) << (i * 8));
    }
    return c;
}

void CFWriteBatch::SetCount(uint32_t n) {
    if (rep_.size() < 12) rep_.resize(12, 0);
    uint8_t* p = reinterpret_cast<uint8_t*>(rep_.data());
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>((n >> (i * 8)) & 0xFF);
    }
}

SequenceNumber CFWriteBatch::Sequence() const {
    if (rep_.size() < 12) return 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(rep_.data() + 4);
    uint64_t seq = 0;
    for (int i = 0; i < 8; ++i) {
        seq |= (static_cast<uint64_t>(p[i]) << (i * 8));
    }
    return seq;
}

void CFWriteBatch::SetSequence(SequenceNumber seq) {
    if (rep_.size() < 12) rep_.resize(12, 0);
    uint8_t* p = reinterpret_cast<uint8_t*>(rep_.data() + 4);
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((seq >> (i * 8)) & 0xFF);
    }
}

void CFWriteBatch::Put(uint32_t cf_id, const Slice& key, const Slice& value) {
    SetCount(Count() + 1);
    rep_.push_back(static_cast<char>(kTypeValue));
    EncodeVarint32(&rep_, cf_id);
    EncodeVarint32(&rep_, static_cast<uint32_t>(key.size()));
    rep_.append(key.data(), key.size());
    EncodeVarint32(&rep_, static_cast<uint32_t>(value.size()));
    rep_.append(value.data(), value.size());
}

void CFWriteBatch::Put(ColumnFamilyHandle* handle, const Slice& key, const Slice& value) {
    uint32_t cf_id = handle ? handle->GetID() : 0;
    Put(cf_id, key, value);
}

void CFWriteBatch::Delete(uint32_t cf_id, const Slice& key) {
    SetCount(Count() + 1);
    rep_.push_back(static_cast<char>(kTypeDeletion));
    EncodeVarint32(&rep_, cf_id);
    EncodeVarint32(&rep_, static_cast<uint32_t>(key.size()));
    rep_.append(key.data(), key.size());
}

void CFWriteBatch::Delete(ColumnFamilyHandle* handle, const Slice& key) {
    uint32_t cf_id = handle ? handle->GetID() : 0;
    Delete(cf_id, key);
}

Status CFWriteBatch::Iterate(Handler* handler) const {
    if (!handler) {
        return Status::InvalidArgument("Null Handler provided to CFWriteBatch::Iterate");
    }

    if (rep_.size() < 12) {
        return Status::Corruption("Corrupt CFWriteBatch header");
    }

    Slice input(rep_.data() + 12, rep_.size() - 12);
    uint32_t expected_count = Count();
    uint32_t found = 0;

    while (!input.empty()) {
        char tag = input[0];
        input.remove_prefix(1);

        uint32_t cf_id = 0;
        if (!DecodeVarint32(&input, &cf_id)) {
            return Status::Corruption("Corrupt cf_id in CFWriteBatch");
        }

        Slice key;
        if (!DecodeLengthPrefixedSlice(&input, &key)) {
            return Status::Corruption("Corrupt key in CFWriteBatch");
        }

        if (tag == static_cast<char>(kTypeValue)) {
            Slice value;
            if (!DecodeLengthPrefixedSlice(&input, &value)) {
                return Status::Corruption("Corrupt value in CFWriteBatch");
            }
            handler->Put(cf_id, key, value);
            found++;
        } else if (tag == static_cast<char>(kTypeDeletion)) {
            handler->Delete(cf_id, key);
            found++;
        } else {
            return Status::Corruption("Unknown record type in CFWriteBatch");
        }
    }

    if (found != expected_count) {
        return Status::Corruption("CFWriteBatch count mismatch");
    }

    return Status::OK();
}

} // namespace lsm
