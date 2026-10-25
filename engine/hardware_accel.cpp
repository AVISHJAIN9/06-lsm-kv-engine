#include "hardware_accel.hpp"
#include <cstring>
#include <sstream>
#include <array>
#include <chrono>

#if defined(__ARM_FEATURE_CRC32) || defined(__aarch64__) || defined(_M_ARM64)
#include <arm_acle.h>
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <nmmintrin.h>
#include <immintrin.h>
#endif

namespace lsm {

namespace {

// Compile-time table generator for Castagnoli CRC32C (0x82F63B78)
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

} // anonymous namespace

const CPUFeatureSet& HardwareFeatures::Get() {
    static CPUFeatureSet features = []() {
        CPUFeatureSet f;
#if defined(__aarch64__) || defined(_M_ARM64)
        f.has_neon = true;
        f.has_hw_crc32c = true;
#elif defined(__x86_64__) || defined(_M_X64)
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            f.has_sse42 = (ecx & (1 << 20)) != 0;
            f.has_hw_crc32c = f.has_sse42;
        }
        if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
            f.has_avx2 = (ebx & (1 << 5)) != 0;
            f.has_avx512 = (ebx & (1 << 16)) != 0;
        }
#endif
        return f;
    }();
    return features;
}

std::string HardwareFeatures::FeatureString() {
    const auto& f = Get();
    std::ostringstream oss;
    oss << "CPU Features [";
    if (f.has_neon) oss << " NEON";
    if (f.has_sse42) oss << " SSE4.2";
    if (f.has_avx2) oss << " AVX2";
    if (f.has_avx512) oss << " AVX-512";
    if (f.has_hw_crc32c) oss << " HW_CRC32C";
    oss << " ]";
    return oss.str();
}

uint32_t SIMDChecksum::ScalarCRC32C(const char* data, size_t n, uint32_t seed) {
    uint32_t crc = ~seed;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    while (n-- > 0) {
        crc = (crc >> 8) ^ kCRC32CTable[(crc & 0xFF) ^ *p++];
    }
    return ~crc;
}

uint32_t SIMDChecksum::CRC32C(const char* data, size_t n, uint32_t seed) {
    uint32_t crc = ~seed;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);

#if defined(__aarch64__) || defined(_M_ARM64)
    while (n >= 64) {
        uint64_t v0, v1, v2, v3, v4, v5, v6, v7;
        std::memcpy(&v0, p, 8);
        std::memcpy(&v1, p + 8, 8);
        std::memcpy(&v2, p + 16, 8);
        std::memcpy(&v3, p + 24, 8);
        std::memcpy(&v4, p + 32, 8);
        std::memcpy(&v5, p + 40, 8);
        std::memcpy(&v6, p + 48, 8);
        std::memcpy(&v7, p + 56, 8);
        crc = __crc32cd(crc, v0);
        crc = __crc32cd(crc, v1);
        crc = __crc32cd(crc, v2);
        crc = __crc32cd(crc, v3);
        crc = __crc32cd(crc, v4);
        crc = __crc32cd(crc, v5);
        crc = __crc32cd(crc, v6);
        crc = __crc32cd(crc, v7);
        p += 64;
        n -= 64;
    }
    while (n >= 8) {
        uint64_t val;
        std::memcpy(&val, p, sizeof(val));
        crc = __crc32cd(crc, val);
        p += 8;
        n -= 8;
    }
    if (n >= 4) {
        uint32_t val;
        std::memcpy(&val, p, sizeof(val));
        crc = __crc32cw(crc, val);
        p += 4;
        n -= 4;
    }
    if (n >= 2) {
        uint16_t val;
        std::memcpy(&val, p, sizeof(val));
        crc = __crc32ch(crc, val);
        p += 2;
        n -= 2;
    }
    if (n >= 1) {
        crc = __crc32cb(crc, *p);
    }
    return ~crc;
#elif defined(__x86_64__) || defined(_M_X64)
    if (HardwareFeatures::Get().has_hw_crc32c) {
        while (n >= 64) {
            uint64_t v0, v1, v2, v3, v4, v5, v6, v7;
            std::memcpy(&v0, p, 8);
            std::memcpy(&v1, p + 8, 8);
            std::memcpy(&v2, p + 16, 8);
            std::memcpy(&v3, p + 24, 8);
            std::memcpy(&v4, p + 32, 8);
            std::memcpy(&v5, p + 40, 8);
            std::memcpy(&v6, p + 48, 8);
            std::memcpy(&v7, p + 56, 8);
            crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v0));
            crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v1));
            crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v2));
            crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v3));
            crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v4));
            crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v5));
            crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v6));
            crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v7));
            p += 64;
            n -= 64;
        }
        while (n >= 8) {
            uint64_t val;
            std::memcpy(&val, p, sizeof(val));
            crc = static_cast<uint32_t>(_mm_crc32_u64(crc, val));
            p += 8;
            n -= 8;
        }
        while (n > 0) {
            crc = _mm_crc32_u8(crc, *p++);
            n--;
        }
        return ~crc;
    }
    return ScalarCRC32C(data, n, seed);
#else
    return ScalarCRC32C(data, n, seed);
#endif
}

uint8_t SIMDBitPacking::BitsNeeded(uint32_t max_val) {
    if (max_val == 0) return 1;
    uint8_t bits = 0;
    while (max_val > 0) {
        bits++;
        max_val >>= 1;
    }
    return bits;
}

size_t SIMDBitPacking::PackDeltas32(
    const uint32_t* src,
    size_t count,
    uint32_t base_val,
    uint8_t bits_per_int,
    uint8_t* dst
) {
    if (count == 0 || !src || !dst) return 0;
    if (bits_per_int == 0) bits_per_int = 1;
    if (bits_per_int > 32) bits_per_int = 32;

    size_t total_bits = count * bits_per_int;
    size_t total_bytes = (total_bits + 7) / 8;
    std::memset(dst, 0, total_bytes);

    size_t bit_pos = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t delta = (src[i] >= base_val) ? (src[i] - base_val) : 0;
        uint64_t val = delta & ((1ULL << bits_per_int) - 1);

        size_t byte_idx = bit_pos / 8;
        size_t bit_offset = bit_pos % 8;

        uint64_t chunk = 0;
        size_t available_bytes = std::min(size_t(8), total_bytes - byte_idx);
        std::memcpy(&chunk, dst + byte_idx, available_bytes);

        chunk |= (val << bit_offset);
        std::memcpy(dst + byte_idx, &chunk, available_bytes);

        bit_pos += bits_per_int;
    }

    return total_bytes;
}

void SIMDBitPacking::UnpackDeltas32(
    const uint8_t* src,
    size_t count,
    uint32_t base_val,
    uint8_t bits_per_int,
    uint32_t* dst
) {
    if (count == 0 || !src || !dst) return;
    if (bits_per_int == 0) bits_per_int = 1;
    if (bits_per_int > 32) bits_per_int = 32;

    uint32_t mask = (bits_per_int == 32) ? 0xffffffffu : ((1u << bits_per_int) - 1);

    size_t bit_pos = 0;
    size_t i = 0;

    // Unroll in 8-element batches for vectorization
    for (; i + 8 <= count; i += 8) {
        for (size_t k = 0; k < 8; ++k) {
            size_t byte_idx = bit_pos / 8;
            size_t bit_offset = bit_pos % 8;

            uint64_t chunk = 0;
            std::memcpy(&chunk, src + byte_idx, 8);
            uint32_t delta = static_cast<uint32_t>((chunk >> bit_offset) & mask);
            dst[i + k] = base_val + delta;
            bit_pos += bits_per_int;
        }
    }

    // Scalar remainder
    for (; i < count; ++i) {
        size_t byte_idx = bit_pos / 8;
        size_t bit_offset = bit_pos % 8;

        uint64_t chunk = 0;
        std::memcpy(&chunk, src + byte_idx, 4);
        uint32_t delta = static_cast<uint32_t>((chunk >> bit_offset) & mask);
        dst[i] = base_val + delta;
        bit_pos += bits_per_int;
    }
}

} // namespace lsm
