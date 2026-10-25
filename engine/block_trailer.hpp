#pragma once

#include <cstdint>
#include <string>
#include "slice.hpp"
#include "status.hpp"
#include "wal_format.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// CompressionType: Supported block compression algorithms
// -----------------------------------------------------------------------------
enum class CompressionType : uint8_t {
    kNoCompression = 0x0,
    kSnappyCompression = 0x1,
    kZSTDCompression = 0x2
};

// Fixed 5-byte block trailer (1 byte compression type + 4 bytes masked CRC32C)
static constexpr size_t kBlockTrailerSize = 5;

// Appends compression type (1 byte) and masked CRC32C (4 bytes) to block buffer
inline void WriteBlockTrailer(std::string* block, CompressionType compression = CompressionType::kNoCompression) {
    const uint8_t comp_byte = static_cast<uint8_t>(compression);
    block->push_back(static_cast<char>(comp_byte));

    // Calculate CRC32C over block content + compression byte
    uint32_t crc = wal::CRC32C::Value(block->data(), block->size());
    uint32_t masked_crc = wal::CRC32C::Mask(crc);

    char crc_buf[4];
    crc_buf[0] = static_cast<char>(masked_crc & 0xff);
    crc_buf[1] = static_cast<char>((masked_crc >> 8) & 0xff);
    crc_buf[2] = static_cast<char>((masked_crc >> 16) & 0xff);
    crc_buf[3] = static_cast<char>((masked_crc >> 24) & 0xff);

    block->append(crc_buf, sizeof(crc_buf));
}

// Verifies CRC32C envelope and isolates corrupted SSTable blocks
inline Status ReadAndVerifyBlock(
    const Slice& raw_block_with_trailer,
    Slice* out_payload,
    CompressionType* out_compression = nullptr
) {
    if (raw_block_with_trailer.size() < kBlockTrailerSize) {
        return Status::Corruption("Block is too short to contain a valid trailer (minimum 5 bytes required)");
    }

    const size_t payload_size = raw_block_with_trailer.size() - kBlockTrailerSize;
    const char* trailer_ptr = raw_block_with_trailer.data() + payload_size;

    const uint8_t comp_byte = static_cast<uint8_t>(trailer_ptr[0]);
    if (comp_byte > static_cast<uint8_t>(CompressionType::kZSTDCompression)) {
        return Status::Corruption("Unknown or unsupported block compression type");
    }

    const uint8_t* const crc_bytes = reinterpret_cast<const uint8_t*>(trailer_ptr + 1);
    const uint32_t expected_masked_crc =
        static_cast<uint32_t>(crc_bytes[0]) |
        (static_cast<uint32_t>(crc_bytes[1]) << 8) |
        (static_cast<uint32_t>(crc_bytes[2]) << 16) |
        (static_cast<uint32_t>(crc_bytes[3]) << 24);

    // Compute CRC32C over payload + 1 byte compression type
    const uint32_t actual_crc = wal::CRC32C::Value(raw_block_with_trailer.data(), payload_size + 1);
    const uint32_t actual_masked_crc = wal::CRC32C::Mask(actual_crc);

    if (actual_masked_crc != expected_masked_crc) {
        return Status::Corruption("Block CRC32C checksum mismatch: block payload is corrupted or damaged");
    }

    if (out_payload) {
        *out_payload = Slice(raw_block_with_trailer.data(), payload_size);
    }
    if (out_compression) {
        *out_compression = static_cast<CompressionType>(comp_byte);
    }

    return Status::OK();
}

} // namespace lsm
