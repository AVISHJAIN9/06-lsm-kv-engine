#include "format.hpp"
#include <cstring>

namespace lsm {

namespace {

static char* EncodeVarint64(char* dst, uint64_t v) {
    uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
    static const int B = 128;
    while (v >= B) {
        *(ptr++) = (v & (B - 1)) | B;
        v >>= 7;
    }
    *(ptr++) = static_cast<uint8_t>(v);
    return reinterpret_cast<char*>(ptr);
}

static const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
    uint64_t result = 0;
    for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
        uint64_t byte = *(reinterpret_cast<const uint8_t*>(p));
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

} // namespace

BlockHandle::BlockHandle() = default;
BlockHandle::BlockHandle(uint64_t offset, uint64_t size)
    : offset_(offset), size_(size) {}

void BlockHandle::EncodeTo(std::string* dst) const {
    char buf[kMaxEncodedLength];
    char* ptr = EncodeVarint64(buf, offset_);
    ptr = EncodeVarint64(ptr, size_);
    dst->append(buf, ptr - buf);
}

Status BlockHandle::DecodeFrom(Slice* input) {
    const char* p = input->data();
    const char* limit = p + input->size();

    p = GetVarint64Ptr(p, limit, &offset_);
    if (p == nullptr) {
        return Status::Corruption("Bad offset in BlockHandle::DecodeFrom");
    }
    p = GetVarint64Ptr(p, limit, &size_);
    if (p == nullptr) {
        return Status::Corruption("Bad size in BlockHandle::DecodeFrom");
    }
    *input = Slice(p, limit - p);
    return Status::OK();
}

void Footer::EncodeTo(std::string* dst) const {
    const size_t original_size = dst->size();
    metaindex_handle_.EncodeTo(dst);
    index_handle_.EncodeTo(dst);
    dst->resize(original_size + 2 * BlockHandle::kMaxEncodedLength, 0); // Padding zeros to 40 bytes

    char magic_buf[8];
    EncodeFixed64(magic_buf, kTableMagicNumber);
    dst->append(magic_buf, sizeof(magic_buf)); // Magic 8 bytes -> Total 48 bytes
}

Status Footer::DecodeFrom(Slice* input) {
    if (input->size() < kEncodedLength) {
        return Status::Corruption("Input is too short to be a valid SSTable Footer (expected 48 bytes)");
    }

    const char* magic_ptr = input->data() + kEncodedLength - 8;
    const uint64_t magic = DecodeFixed64(magic_ptr);
    if (magic != kTableMagicNumber) {
        return Status::Corruption("Invalid SSTable footer magic number: not an SSTable file or corrupt footer");
    }

    Slice handles_slice(input->data(), 2 * BlockHandle::kMaxEncodedLength);
    Status s = metaindex_handle_.DecodeFrom(&handles_slice);
    if (!s.ok()) {
        return s;
    }
    s = index_handle_.DecodeFrom(&handles_slice);
    if (!s.ok()) {
        return s;
    }

    *input = Slice(input->data() + kEncodedLength, input->size() - kEncodedLength);
    return Status::OK();
}

} // namespace lsm
