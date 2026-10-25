#pragma once

#include <string>
#include <cstdint>
#include "status.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// SSTableCorruptor: Programmatic fault and bit-rot injector for robustness tests
// -----------------------------------------------------------------------------
class SSTableCorruptor {
public:
    // Flips bits at a specific byte offset
    static Status FlipBit(const std::string& filename, uint64_t byte_offset, uint8_t bit_mask = 0x01);

    // Corrupts the 4-byte CRC32C in the trailer of the block at block_offset
    static Status CorruptBlockCRC(const std::string& filename, uint64_t block_end_offset);

    // Truncates the last N bytes of the file (e.g. damaging the 48-byte footer)
    static Status TruncateFile(const std::string& filename, uint64_t bytes_to_remove);

    // Corrupts the 8-byte table magic constant in the footer
    static Status CorruptMagicNumber(const std::string& filename);

    // Overwrites a byte range with zeroes
    static Status ZeroRange(const std::string& filename, uint64_t offset, size_t length);
};

} // namespace lsm
