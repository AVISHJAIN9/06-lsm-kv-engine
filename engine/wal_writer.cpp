#include "wal_writer.hpp"
#include <cassert>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>

namespace lsm {

PosixWritableFile::PosixWritableFile(std::string filename, int fd)
    : filename_(std::move(filename)), fd_(fd), file_size_(0), buffer_offset_(0) {
    if (fd_ >= 0) {
        struct stat s;
        if (fstat(fd_, &s) == 0) {
            file_size_ = static_cast<uint64_t>(s.st_size);
        }
    }
}

PosixWritableFile::~PosixWritableFile() {
    if (fd_ >= 0) {
        Close();
    }
}

Status PosixWritableFile::FlushBuffer() {
    if (buffer_offset_ == 0) {
        return Status::OK();
    }

    const char* p = buffer_;
    size_t left = buffer_offset_;
    while (left > 0) {
        ssize_t written = write(fd_, p, left);
        if (written < 0) {
            if (errno == EINTR) continue;
            return Status::IOError("Failed to write to file: " + filename_);
        }
        p += written;
        left -= written;
    }
    buffer_offset_ = 0;
    return Status::OK();
}

Status PosixWritableFile::Append(const Slice& data) {
    size_t left = data.size();
    const char* src = data.data();

    file_size_ += left;

    // Fast path: fits in write buffer
    if (buffer_offset_ + left <= sizeof(buffer_)) {
        std::memcpy(buffer_ + buffer_offset_, src, left);
        buffer_offset_ += left;
        return Status::OK();
    }

    // Flush buffer if partially filled
    if (buffer_offset_ > 0) {
        Status s = FlushBuffer();
        if (!s.ok()) return s;
    }

    // Direct write if data chunk >= buffer size
    if (left >= sizeof(buffer_)) {
        while (left > 0) {
            ssize_t written = write(fd_, src, left);
            if (written < 0) {
                if (errno == EINTR) continue;
                return Status::IOError("Failed to write large chunk to file: " + filename_);
            }
            src += written;
            left -= written;
        }
        return Status::OK();
    }

    // Copy remaining to fresh buffer
    std::memcpy(buffer_, src, left);
    buffer_offset_ = left;
    return Status::OK();
}

Status PosixWritableFile::Flush() {
    return FlushBuffer();
}

Status PosixWritableFile::Sync() {
    Status s = FlushBuffer();
    if (!s.ok()) return s;

    if (fsync(fd_) < 0) {
        return Status::IOError("fsync failed for: " + filename_);
    }
    return Status::OK();
}

Status PosixWritableFile::Close() {
    Status s = FlushBuffer();
    if (fd_ >= 0) {
        if (close(fd_) < 0 && s.ok()) {
            s = Status::IOError("Failed to close file: " + filename_);
        }
        fd_ = -1;
    }
    return s;
}

WALWriter::WALWriter(std::unique_ptr<WritableFile> dest, uint64_t log_number)
    : dest_(std::move(dest)), log_number_(log_number), block_offset_(0) {
    for (int i = 0; i <= wal::kMaxRecordType; ++i) {
        char type_byte = static_cast<char>(i);
        type_crc_[i] = wal::CRC32C::Value(&type_byte, 1);
    }
}

WALWriter::WALWriter(const std::string& filepath, uint64_t log_number)
    : log_number_(log_number), block_offset_(0) {
    int fd = ::open(filepath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dest_ = std::make_unique<PosixWritableFile>(filepath, fd);
    }
    for (int i = 0; i <= wal::kMaxRecordType; ++i) {
        char type_byte = static_cast<char>(i);
        type_crc_[i] = wal::CRC32C::Value(&type_byte, 1);
    }
}

WALWriter::~WALWriter() {
    Close();
}

Status WALWriter::Flush() {
    if (dest_) {
        return dest_->Flush();
    }
    return Status::OK();
}

Status WALWriter::Sync() {
    if (dest_) {
        return dest_->Sync();
    }
    return Status::OK();
}

Status WALWriter::Close() {
    if (dest_) {
        Status s = dest_->Close();
        dest_.reset();
        return s;
    }
    return Status::OK();
}

Status WALWriter::AddRecord(const Slice& slice) {
    const char* ptr = slice.data();
    size_t left = slice.size();

    // Fragment the record if it does not fit in the remaining space of the current block
    Status s;
    bool begin = true;
    do {
        const int leftover = static_cast<int>(wal::kBlockSize) - block_offset_;
        assert(leftover >= 0);
        if (leftover < static_cast<int>(wal::kHeaderSize)) {
            // Fill remaining block with zero padding
            if (leftover > 0) {
                static const char zeroes[wal::kHeaderSize] = {0};
                s = dest_->Append(Slice(zeroes, static_cast<size_t>(leftover)));
                if (!s.ok()) return s;
            }
            block_offset_ = 0;
        }

        // Available space in current block for payload
        assert(wal::kBlockSize >= block_offset_ + wal::kHeaderSize);
        const size_t avail = wal::kBlockSize - block_offset_ - wal::kHeaderSize;
        const size_t fragment_length = (left < avail) ? left : avail;

        const bool end = (left == fragment_length);
        wal::RecordType type;
        if (begin && end) {
            type = wal::kFullType;
        } else if (begin) {
            type = wal::kFirstType;
        } else if (end) {
            type = wal::kLastType;
        } else {
            type = wal::kMiddleType;
        }

        s = EmitPhysicalRecord(type, ptr, fragment_length);
        ptr += fragment_length;
        left -= fragment_length;
        begin = false;
    } while (s.ok() && left > 0);

    return s;
}

Status WALWriter::EmitPhysicalRecord(wal::RecordType type, const char* ptr, size_t length) {
    assert(length <= 0xffff); // Length must fit in 16 bits
    assert(block_offset_ + wal::kHeaderSize + length <= wal::kBlockSize);

    // Header: 4 bytes CRC + 2 bytes Length + 1 byte Type
    char buf[wal::kHeaderSize];
    buf[4] = static_cast<char>(length & 0xff);
    buf[5] = static_cast<char>((length >> 8) & 0xff);
    buf[6] = static_cast<char>(type);

    // Compute CRC over type and payload
    uint32_t crc = wal::CRC32C::Extend(type_crc_[type], ptr, length);
    crc = wal::CRC32C::Mask(crc);
    buf[0] = static_cast<char>(crc & 0xff);
    buf[1] = static_cast<char>((crc >> 8) & 0xff);
    buf[2] = static_cast<char>((crc >> 16) & 0xff);
    buf[3] = static_cast<char>((crc >> 24) & 0xff);

    // Write header
    Status s = dest_->Append(Slice(buf, wal::kHeaderSize));
    if (s.ok()) {
        s = dest_->Append(Slice(ptr, length));
        if (s.ok()) {
            block_offset_ += static_cast<int>(wal::kHeaderSize + length);
            if (block_offset_ == static_cast<int>(wal::kBlockSize)) {
                block_offset_ = 0;
            }
        }
    }
    return s;
}

} // namespace lsm
