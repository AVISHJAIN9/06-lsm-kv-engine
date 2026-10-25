#include "snappy_codec.hpp"
#include <cstring>
#include <vector>
#include <algorithm>

namespace lsm {

namespace {

inline void PutVarint32(std::string* dst, uint32_t v) {
    char buf[5];
    char* ptr = buf;
    while (v >= 128) {
        *(ptr++) = static_cast<char>((v & 127) | 128);
        v >>= 7;
    }
    *(ptr++) = static_cast<char>(v);
    dst->append(buf, ptr - buf);
}

inline bool GetVarint32(const char** p, const char* limit, uint32_t* value) {
    uint32_t result = 0;
    for (uint32_t shift = 0; shift <= 28 && *p < limit; shift += 7) {
        uint32_t byte = static_cast<uint8_t>(*(*p)++);
        if (byte & 128) {
            result |= ((byte & 127) << shift);
        } else {
            result |= (byte << shift);
            *value = result;
            return true;
        }
    }
    return false;
}

inline uint32_t Hash4(const char* p) {
    uint32_t val;
    std::memcpy(&val, p, sizeof(val));
    return (val * 0x1e35a7bd) >> 18; // 14-bit table index (16384 entries)
}

inline void EmitLiteral(std::string* out, const char* lit, size_t len) {
    if (len == 0) return;
    if (len <= 60) {
        out->push_back(static_cast<char>((len - 1) << 2));
    } else if (len <= 256) {
        out->push_back(static_cast<char>(60 << 2));
        out->push_back(static_cast<char>(len - 1));
    } else {
        out->push_back(static_cast<char>(61 << 2));
        out->push_back(static_cast<char>((len - 1) & 0xff));
        out->push_back(static_cast<char>(((len - 1) >> 8) & 0xff));
    }
    out->append(lit, len);
}

inline void EmitCopy(std::string* out, size_t offset, size_t len) {
    while (len > 0) {
        if (len < 12 && offset < 2048) {
            size_t copy_len = std::min(len, size_t(11));
            uint8_t tag = static_cast<uint8_t>(0x01 | ((copy_len - 4) << 2) | ((offset >> 8) << 5));
            out->push_back(static_cast<char>(tag));
            out->push_back(static_cast<char>(offset & 0xff));
            len -= copy_len;
            break;
        } else {
            size_t copy_len = std::min(len, size_t(64));
            uint8_t tag = static_cast<uint8_t>(0x02 | ((copy_len - 1) << 2));
            out->push_back(static_cast<char>(tag));
            out->push_back(static_cast<char>(offset & 0xff));
            out->push_back(static_cast<char>((offset >> 8) & 0xff));
            len -= copy_len;
        }
    }
}

} // anonymous namespace

size_t SnappyCodec::MaxCompressedLength(size_t input_len) const {
    return 32 + input_len + input_len / 6;
}

Status SnappyCodec::Compress(
    const Slice& input,
    std::string* output,
    const CompressionOptions& opts
) {
    (void)opts;
    if (!output) return Status::InvalidArgument("Null output buffer");

    output->clear();
    PutVarint32(output, static_cast<uint32_t>(input.size()));

    const size_t input_size = input.size();
    if (input_size == 0) return Status::OK();

    const char* src = input.data();
    const char* ip = src;
    const char* ip_limit = src + input_size;
    const char* next_emit = src;

    if (input_size >= 15) {
        constexpr size_t kTableSize = 16384;
        uint16_t table[kTableSize];
        std::memset(table, 0, sizeof(table));

        const char* ip_fast_limit = ip_limit - 15;
        while (ip <= ip_fast_limit) {
            uint32_t hash = Hash4(ip);
            const char* candidate = src + table[hash];
            table[hash] = static_cast<uint16_t>(ip - src);

            if (candidate >= src && candidate < ip &&
                (ip - candidate) < 65535 &&
                std::memcmp(candidate, ip, 4) == 0) {
                // Match found!
                EmitLiteral(output, next_emit, ip - next_emit);

                // Find match length
                const char* match_src = candidate + 4;
                const char* match_ip = ip + 4;
                while (match_ip < ip_limit && *match_src == *match_ip) {
                    match_src++;
                    match_ip++;
                }
                size_t match_len = match_ip - ip;
                size_t offset = ip - candidate;
                EmitCopy(output, offset, match_len);

                ip = match_ip;
                next_emit = ip;
            } else {
                ip++;
            }
        }
    }

    // Emit trailing literal
    if (next_emit < ip_limit) {
        EmitLiteral(output, next_emit, ip_limit - next_emit);
    }

    return Status::OK();
}

Status SnappyCodec::Uncompress(
    const Slice& input,
    char* uncompressed_buf,
    size_t uncompressed_len
) {
    if (!uncompressed_buf && uncompressed_len > 0) {
        return Status::InvalidArgument("Null destination buffer");
    }

    const char* ip = input.data();
    const char* ip_limit = input.data() + input.size();

    uint32_t expected_len = 0;
    if (!GetVarint32(&ip, ip_limit, &expected_len)) {
        return Status::Corruption("Snappy: Invalid varint length header");
    }

    if (expected_len != uncompressed_len) {
        return Status::Corruption("Snappy: Length mismatch (header=" +
                                  std::to_string(expected_len) + ", expected=" +
                                  std::to_string(uncompressed_len) + ")");
    }

    char* op = uncompressed_buf;
    char* op_limit = uncompressed_buf + uncompressed_len;

    while (ip < ip_limit && op < op_limit) {
        uint8_t tag = static_cast<uint8_t>(*ip++);
        uint8_t tag_type = tag & 0x03;

        switch (tag_type) {
            case 0x00: { // Literal
                size_t len = (tag >> 2) + 1;
                if (len > 60) {
                    size_t extra_bytes = len - 60;
                    if (extra_bytes > 4 || ip + extra_bytes > ip_limit) {
                        return Status::Corruption("Snappy: Truncated literal length");
                    }
                    len = 0;
                    for (size_t b = 0; b < extra_bytes; ++b) {
                        len |= (static_cast<uint8_t>(ip[b]) << (b * 8));
                    }
                    len += 1;
                    ip += extra_bytes;
                }
                if (ip + len > ip_limit || op + len > op_limit) {
                    return Status::Corruption("Snappy: Literal buffer overrun");
                }
                std::memcpy(op, ip, len);
                ip += len;
                op += len;
                break;
            }
            case 0x01: { // 1-byte copy
                if (ip >= ip_limit) return Status::Corruption("Snappy: Truncated copy header");
                size_t len = ((tag >> 2) & 0x07) + 4;
                size_t offset = ((tag >> 5) << 8) | static_cast<uint8_t>(*ip++);
                if (offset == 0 || offset > static_cast<size_t>(op - uncompressed_buf)) {
                    return Status::Corruption("Snappy: Invalid copy offset");
                }
                if (op + len > op_limit) {
                    return Status::Corruption("Snappy: Copy destination overrun");
                }
                const char* copy_src = op - offset;
                for (size_t k = 0; k < len; ++k) {
                    *op++ = *copy_src++;
                }
                break;
            }
            case 0x02: { // 2-byte copy
                if (ip + 2 > ip_limit) return Status::Corruption("Snappy: Truncated 2-byte copy header");
                size_t len = (tag >> 2) + 1;
                size_t offset = static_cast<uint8_t>(ip[0]) | (static_cast<uint8_t>(ip[1]) << 8);
                ip += 2;
                if (offset == 0 || offset > static_cast<size_t>(op - uncompressed_buf)) {
                    return Status::Corruption("Snappy: Invalid 2-byte copy offset");
                }
                if (op + len > op_limit) {
                    return Status::Corruption("Snappy: Copy destination overrun");
                }
                const char* copy_src = op - offset;
                for (size_t k = 0; k < len; ++k) {
                    *op++ = *copy_src++;
                }
                break;
            }
            case 0x03: { // 4-byte copy
                if (ip + 4 > ip_limit) return Status::Corruption("Snappy: Truncated 4-byte copy header");
                size_t len = (tag >> 2) + 1;
                size_t offset = static_cast<uint8_t>(ip[0]) |
                                (static_cast<uint8_t>(ip[1]) << 8) |
                                (static_cast<uint8_t>(ip[2]) << 16) |
                                (static_cast<uint8_t>(ip[3]) << 24);
                ip += 4;
                if (offset == 0 || offset > static_cast<size_t>(op - uncompressed_buf)) {
                    return Status::Corruption("Snappy: Invalid 4-byte copy offset");
                }
                if (op + len > op_limit) {
                    return Status::Corruption("Snappy: Copy destination overrun");
                }
                const char* copy_src = op - offset;
                for (size_t k = 0; k < len; ++k) {
                    *op++ = *copy_src++;
                }
                break;
            }
        }
    }

    if (op != op_limit || ip != ip_limit) {
        return Status::Corruption("Snappy: Decompressed size did not match header or trailing garbage");
    }

    return Status::OK();
}

} // namespace lsm
