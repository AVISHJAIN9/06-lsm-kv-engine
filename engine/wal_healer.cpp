#include "wal_healer.hpp"
#include "wal_format.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <vector>
#include <sstream>

namespace lsm {

Status WALHealer::TruncateToOffset(const std::string& filepath, uint64_t valid_offset) {
    if (::truncate(filepath.c_str(), static_cast<off_t>(valid_offset)) != 0) {
        return Status::IOError("Failed to truncate file: " + filepath + " to " + std::to_string(valid_offset) + " bytes: " + strerror(errno));
    }
    return Status::OK();
}

bool WALHealer::ValidateBlockRecord(
    const uint8_t* block_data,
    size_t block_len,
    size_t offset_in_block,
    uint8_t* out_record_type,
    uint16_t* out_record_len,
    uint32_t* out_expected_crc
) {
    if (offset_in_block + wal::kHeaderSize > block_len) {
        return false;
    }

    const uint8_t* p = block_data + offset_in_block;
    uint32_t stored_crc = static_cast<uint32_t>(p[0]) |
                         (static_cast<uint32_t>(p[1]) << 8) |
                         (static_cast<uint32_t>(p[2]) << 16) |
                         (static_cast<uint32_t>(p[3]) << 24);
    uint16_t length = static_cast<uint16_t>(p[4]) | (static_cast<uint16_t>(p[5]) << 8);
    uint8_t type = p[6];

    if (out_record_type) *out_record_type = type;
    if (out_record_len) *out_record_len = length;
    if (out_expected_crc) *out_expected_crc = stored_crc;

    if (type == static_cast<uint8_t>(wal::RecordType::kZero)) {
        // Zero padding record
        return true;
    }

    if (offset_in_block + wal::kHeaderSize + length > block_len) {
        return false;
    }

    // Compute CRC over type (1 byte) + payload (length bytes)
    uint32_t computed_crc = wal::CRC32C::Compute(reinterpret_cast<const char*>(p + 6), 1 + length);
    computed_crc = wal::CRC32C::Mask(computed_crc);

    return (computed_crc == stored_crc);
}

Status WALHealer::DetectAndHeal(
    const std::string& filepath,
    bool apply_truncation,
    HealResult* out_result
) {
    if (!out_result) {
        return Status::InvalidArgument("Null HealResult pointer");
    }

    *out_result = HealResult{};

    struct stat st;
    if (::stat(filepath.c_str(), &st) != 0) {
        return Status::IOError("Cannot stat file: " + filepath);
    }

    out_result->original_size = static_cast<uint64_t>(st.st_size);
    if (st.st_size == 0) {
        out_result->valid_byte_offset = 0;
        return Status::OK();
    }

    int fd = ::open(filepath.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status::IOError("Cannot open file for reading: " + filepath);
    }

    std::vector<uint8_t> block_buf(wal::kBlockSize);
    uint64_t file_offset = 0;
    uint64_t last_valid_record_end_file_offset = 0;
    size_t valid_records_count = 0;
    bool torn_found = false;

    while (file_offset < static_cast<uint64_t>(st.st_size)) {
        ssize_t bytes_read = ::read(fd, block_buf.data(), wal::kBlockSize);
        if (bytes_read <= 0) {
            break;
        }

        size_t block_bytes = static_cast<size_t>(bytes_read);
        size_t offset_in_block = 0;
        bool block_corrupted = false;

        while (offset_in_block < block_bytes) {
            size_t remaining_in_block = block_bytes - offset_in_block;
            if (remaining_in_block < wal::kHeaderSize) {
                // Check if remaining bytes in block are all zero (normal block boundary padding)
                bool all_zero = true;
                for (size_t i = offset_in_block; i < block_bytes; ++i) {
                    if (block_buf[i] != 0) {
                        all_zero = false;
                        break;
                    }
                }
                if (all_zero) {
                    // Valid padding up to end of block
                    offset_in_block = block_bytes;
                    // If full 32KB block was read, next block starts at next block boundary
                    if (block_bytes == wal::kBlockSize) {
                        last_valid_record_end_file_offset = file_offset + block_bytes;
                    }
                    break;
                } else {
                    torn_found = true;
                    block_corrupted = true;
                    break;
                }
            }

            // Inspect record header
            const uint8_t* hdr = block_buf.data() + offset_in_block;
            uint32_t stored_crc = static_cast<uint32_t>(hdr[0]) |
                                 (static_cast<uint32_t>(hdr[1]) << 8) |
                                 (static_cast<uint32_t>(hdr[2]) << 16) |
                                 (static_cast<uint32_t>(hdr[3]) << 24);
            uint16_t length = static_cast<uint16_t>(hdr[4]) | (static_cast<uint16_t>(hdr[5]) << 8);
            uint8_t type = hdr[6];

            if (type == static_cast<uint8_t>(wal::RecordType::kZero)) {
                // Check if rest of block is all zeros
                bool all_zeros = true;
                for (size_t i = offset_in_block; i < block_bytes; ++i) {
                    if (block_buf[i] != 0) {
                        all_zeros = false;
                        break;
                    }
                }
                if (all_zeros) {
                    offset_in_block = block_bytes;
                    if (block_bytes == wal::kBlockSize) {
                        last_valid_record_end_file_offset = file_offset + block_bytes;
                    }
                    break;
                } else {
                    torn_found = true;
                    block_corrupted = true;
                    break;
                }
            }

            if (type > static_cast<uint8_t>(wal::RecordType::kLast)) {
                // Invalid record type -> torn write
                torn_found = true;
                block_corrupted = true;
                break;
            }

            if (offset_in_block + wal::kHeaderSize + length > block_bytes) {
                // Payload exceeds block boundary -> torn write / partial flush
                torn_found = true;
                block_corrupted = true;
                break;
            }

            // Check CRC
            uint32_t computed_crc = wal::CRC32C::Compute(
                reinterpret_cast<const char*>(hdr + 6),
                1 + length
            );
            computed_crc = wal::CRC32C::Mask(computed_crc);

            if (computed_crc != stored_crc) {
                // CRC mismatch -> corrupted record
                torn_found = true;
                block_corrupted = true;
                break;
            }

            // Valid record verified
            valid_records_count++;
            offset_in_block += wal::kHeaderSize + length;
            last_valid_record_end_file_offset = file_offset + offset_in_block;
        }

        if (block_corrupted) {
            break;
        }

        file_offset += block_bytes;
    }

    ::close(fd);

    out_result->torn_detected = torn_found || (last_valid_record_end_file_offset < out_result->original_size);
    out_result->valid_byte_offset = last_valid_record_end_file_offset;
    out_result->valid_records_preserved = valid_records_count;

    if (out_result->valid_byte_offset < out_result->original_size) {
        out_result->bytes_truncated = out_result->original_size - out_result->valid_byte_offset;
    } else {
        out_result->bytes_truncated = 0;
    }

    std::ostringstream ss;
    ss << "Scanned " << out_result->original_size << " bytes, valid boundary at "
       << out_result->valid_byte_offset << " bytes, valid records: "
       << out_result->valid_records_preserved << ", torn detected: "
       << (out_result->torn_detected ? "true" : "false");
    out_result->details = ss.str();

    if (apply_truncation && out_result->torn_detected && (out_result->bytes_truncated > 0)) {
        Status s = TruncateToOffset(filepath, out_result->valid_byte_offset);
        if (!s.ok()) {
            return s;
        }
    }

    return Status::OK();
}

} // namespace lsm
