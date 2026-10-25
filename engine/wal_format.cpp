#include "wal_format.hpp"
#include <array>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <nmmintrin.h>
#endif

namespace lsm {
namespace wal {

namespace {

#if !(defined(__aarch64__) && defined(__ARM_FEATURE_CRC32))
// Compile-time table generator for CRC32C (Castagnoli 0x82F63B78)
constexpr auto GenerateCRC32CTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78u : 0);
        }
        table[i] = crc;
    }
    return table;
}

static constexpr auto kCRC32CTable = GenerateCRC32CTable();
#endif

} // namespace

uint32_t CRC32C::Extend(uint32_t init_crc, const char* data, size_t n) {
    uint32_t crc = ~init_crc;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    while (n >= 8) {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        crc = __crc32cd(crc, v);
        p += 8;
        n -= 8;
    }
    if (n >= 4) {
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        crc = __crc32cw(crc, v);
        p += 4;
        n -= 4;
    }
    if (n >= 2) {
        uint16_t v;
        std::memcpy(&v, p, sizeof(v));
        crc = __crc32ch(crc, v);
        p += 2;
        n -= 2;
    }
    if (n >= 1) {
        crc = __crc32cb(crc, *p);
        p += 1;
        n -= 1;
    }
#else
    while (n-- > 0) {
        crc = (crc >> 8) ^ kCRC32CTable[(crc & 0xFF) ^ *p++];
    }
#endif

    return ~crc;
}

uint32_t CRC32C::Value(const char* data, size_t n) {
    return Extend(0, data, n);
}

} // namespace wal
} // namespace lsm
