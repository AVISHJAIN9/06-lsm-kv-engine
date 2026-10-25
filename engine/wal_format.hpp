#pragma once

#include <cstdint>
#include <cstddef>
#include "slice.hpp"

namespace lsm {
namespace wal {

static constexpr size_t kBlockSize = 32768; // 32KB
static constexpr size_t kHeaderSize = 4 + 2 + 1; // 7 bytes (4 CRC, 2 Length, 1 Type)

enum RecordType : uint8_t {
    kZero = 0,
    kZeroType = 0,
    kFull = 1,
    kFullType = 1,
    kFirst = 2,
    kFirstType = 2,
    kMiddle = 3,
    kMiddleType = 3,
    kLast = 4,
    kLastType = 4,
    kMaxRecordType = 4
};

// CRC32C implementation (Castagnoli polynomial 0x82F63B78)
class CRC32C {
public:
    static uint32_t Value(const char* data, size_t n);
    static uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

    static uint32_t Compute(const char* data, size_t n) {
        return Value(data, n);
    }

    // Masking function: avoids checksums of zeros matching and handles bit-flips
    static uint32_t Mask(uint32_t crc) {
        return ((crc >> 15) | (crc << 17)) + 0xa282ead8ul;
    }

    static uint32_t Unmask(uint32_t masked_crc) {
        uint32_t rot = masked_crc - 0xa282ead8ul;
        return ((rot >> 17) | (rot << 15));
    }
};

} // namespace wal
} // namespace lsm
