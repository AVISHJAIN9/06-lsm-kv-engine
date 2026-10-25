#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace lsm {

struct CPUFeatureSet {
    bool has_sse42{false};
    bool has_avx2{false};
    bool has_avx512{false};
    bool has_neon{false};
    bool has_hw_crc32c{false};
};

// HardwareFeatures: Probes host CPU instruction capabilities at engine startup (Feature 114).
class HardwareFeatures {
public:
    static const CPUFeatureSet& Get();
    static std::string FeatureString();
};

// SIMDChecksum: Hardware-accelerated CRC32C computation achieving >= 8 GB/sec (Feature 114).
class SIMDChecksum {
public:
    // Computes Castagnoli CRC32C using hardware acceleration when available
    static uint32_t CRC32C(const char* data, size_t n, uint32_t seed = 0);

    // Scalar fallback reference for verification
    static uint32_t ScalarCRC32C(const char* data, size_t n, uint32_t seed = 0);
};

// SIMDBitPacking: Vectorized integer delta bit-packing for restart offsets and sequence numbers (Feature 114).
class SIMDBitPacking {
public:
    // Computes bit width needed to store maximum value in array
    static uint8_t BitsNeeded(uint32_t max_val);

    // Packs count 32-bit integers relative to base_val using exactly bits_per_int bits
    static size_t PackDeltas32(
        const uint32_t* src,
        size_t count,
        uint32_t base_val,
        uint8_t bits_per_int,
        uint8_t* dst
    );

    // Vectorized/unrolled unpacking of delta-encoded integers
    static void UnpackDeltas32(
        const uint8_t* src,
        size_t count,
        uint32_t base_val,
        uint8_t bits_per_int,
        uint32_t* dst
    );
};

} // namespace lsm
