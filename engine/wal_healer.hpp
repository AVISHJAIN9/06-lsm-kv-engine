#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include "status.hpp"
#include "slice.hpp"

namespace lsm {

struct HealResult {
    bool torn_detected{false};
    uint64_t original_size{0};
    uint64_t valid_byte_offset{0};
    uint64_t bytes_truncated{0};
    size_t valid_records_preserved{0};
    std::string details{""};
};

class WALHealer {
public:
    WALHealer() = default;
    ~WALHealer() = default;

    // Inspects a WAL file from the start, validating 32KB blocks, record headers,
    // and CRC32C checksums. Identifies any torn write or partial garbage at the tail.
    // If a torn write is found at the tail and nothing valid follows, it truncates
    // the file to the last valid byte offset via POSIX truncate().
    static Status DetectAndHeal(
        const std::string& filepath,
        bool apply_truncation,
        HealResult* out_result
    );

    // Explicit truncation helper
    static Status TruncateToOffset(const std::string& filepath, uint64_t valid_offset);

    // Validates block CRC32C and record integrity within a single buffer
    static bool ValidateBlockRecord(
        const uint8_t* block_data,
        size_t block_len,
        size_t offset_in_block,
        uint8_t* out_record_type,
        uint16_t* out_record_len,
        uint32_t* out_expected_crc
    );
};

} // namespace lsm
