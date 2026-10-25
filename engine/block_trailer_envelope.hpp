#pragma once

#include <cstdint>
#include <string>
#include "slice.hpp"
#include "status.hpp"
#include "wal_format.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// BlockTrailerEnvelope: Standardized physical 5-byte block framing
// Layout: [Payload: N Bytes] [CompressionType: 1 Byte] [MaskedCRC32C: 4 Bytes]
// -----------------------------------------------------------------------------
class BlockTrailerEnvelope {
public:
    enum class CompressionType : uint8_t {
        kNoCompression = 0x0,
        kSnappyCompression = 0x1,
        kZSTDCompression = 0x2
    };

    static constexpr size_t kTrailerSize = 5;

    // Packs and appends the 5-byte trailer to the block buffer
    static void PackTrailer(std::string* block, CompressionType comp_type = CompressionType::kNoCompression) {
        const uint8_t type_byte = static_cast<uint8_t>(comp_type);
        block->push_back(static_cast<char>(type_byte));

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

    // Verifies the block CRC32C checksum and strips the 5-byte trailer
    static Status VerifyAndStripTrailer(
        const Slice& raw_block_with_trailer,
        Slice* out_payload,
        CompressionType* out_comp_type = nullptr
    ) {
        if (raw_block_with_trailer.size() < kTrailerSize) {
            return Status::Corruption("Block is too short to contain a valid 5-byte trailer envelope");
        }

        const size_t payload_size = raw_block_with_trailer.size() - kTrailerSize;
        const char* trailer_ptr = raw_block_with_trailer.data() + payload_size;

        const uint8_t comp_byte = static_cast<uint8_t>(trailer_ptr[0]);
        if (comp_byte > static_cast<uint8_t>(CompressionType::kZSTDCompression)) {
            return Status::Corruption("Unknown or unsupported block compression type in trailer");
        }

        const uint8_t* const crc_bytes = reinterpret_cast<const uint8_t*>(trailer_ptr + 1);
        const uint32_t expected_masked_crc =
            static_cast<uint32_t>(crc_bytes[0]) |
            (static_cast<uint32_t>(crc_bytes[1]) << 8) |
            (static_cast<uint32_t>(crc_bytes[2]) << 16) |
            (static_cast<uint32_t>(crc_bytes[3]) << 24);

        // Compute actual CRC32C over payload + compression byte
        const uint32_t actual_crc = wal::CRC32C::Value(raw_block_with_trailer.data(), payload_size + 1);
        const uint32_t actual_masked_crc = wal::CRC32C::Mask(actual_crc);

        if (actual_masked_crc != expected_masked_crc) {
            return Status::Corruption("Block CRC32C mismatch: block data has suffered bit rot or corruption");
        }

        if (out_payload) {
            *out_payload = Slice(raw_block_with_trailer.data(), payload_size);
        }
        if (out_comp_type) {
            *out_comp_type = static_cast<CompressionType>(comp_byte);
        }

        return Status::OK();
    }
};

} // namespace lsm
