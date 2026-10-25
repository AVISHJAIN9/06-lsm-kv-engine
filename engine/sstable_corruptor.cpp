#include "sstable_corruptor.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>

namespace lsm {

namespace fs = std::filesystem;

Status SSTableCorruptor::FlipBit(const std::string& filename, uint64_t byte_offset, uint8_t bit_mask) {
    int fd = open(filename.c_str(), O_RDWR);
    if (fd < 0) {
        return Status::IOError("Failed to open SSTable for bit corruption: " + filename);
    }

    uint8_t byte = 0;
    ssize_t n = pread(fd, &byte, 1, byte_offset);
    if (n != 1) {
        close(fd);
        return Status::IOError("Failed to read byte at offset " + std::to_string(byte_offset));
    }

    byte ^= bit_mask;
    n = pwrite(fd, &byte, 1, byte_offset);
    close(fd);

    if (n != 1) {
        return Status::IOError("Failed to write corrupted byte at offset " + std::to_string(byte_offset));
    }
    return Status::OK();
}

Status SSTableCorruptor::CorruptBlockCRC(const std::string& filename, uint64_t block_end_offset) {
    // CRC is stored in the last 4 bytes before block_end_offset
    if (block_end_offset < 4) {
        return Status::InvalidArgument("Invalid block_end_offset < 4");
    }
    return FlipBit(filename, block_end_offset - 2, 0xAA);
}

Status SSTableCorruptor::TruncateFile(const std::string& filename, uint64_t bytes_to_remove) {
    std::error_code ec;
    uint64_t size = fs::file_size(filename, ec);
    if (ec) {
        return Status::IOError("Failed to get file size: " + filename);
    }
    if (bytes_to_remove >= size) {
        return Status::InvalidArgument("bytes_to_remove exceeds file size");
    }

    int fd = open(filename.c_str(), O_RDWR);
    if (fd < 0) {
        return Status::IOError("Failed to open file for truncation: " + filename);
    }

    int res = ftruncate(fd, size - bytes_to_remove);
    close(fd);

    if (res != 0) {
        return Status::IOError("ftruncate failed on " + filename);
    }
    return Status::OK();
}

Status SSTableCorruptor::CorruptMagicNumber(const std::string& filename) {
    std::error_code ec;
    uint64_t size = fs::file_size(filename, ec);
    if (ec || size < 8) {
        return Status::IOError("Invalid file size for magic number corruption");
    }
    // Magic number is at size - 8 .. size
    return FlipBit(filename, size - 4, 0xFF);
}

Status SSTableCorruptor::ZeroRange(const std::string& filename, uint64_t offset, size_t length) {
    int fd = open(filename.c_str(), O_RDWR);
    if (fd < 0) {
        return Status::IOError("Failed to open file for zeroing: " + filename);
    }

    std::string zeros(length, '\0');
    ssize_t n = pwrite(fd, zeros.data(), length, offset);
    close(fd);

    if (n != static_cast<ssize_t>(length)) {
        return Status::IOError("Failed to write zero range to " + filename);
    }
    return Status::OK();
}

} // namespace lsm
