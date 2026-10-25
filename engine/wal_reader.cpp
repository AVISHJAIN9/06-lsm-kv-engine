#include "wal_reader.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace lsm {

PosixSequentialFile::PosixSequentialFile(std::string filename, int fd)
    : filename_(std::move(filename)), fd_(fd) {}

PosixSequentialFile::~PosixSequentialFile() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

Status PosixSequentialFile::Read(size_t n, Slice* result, char* scratch) {
    Status s;
    while (true) {
        ssize_t r = read(fd_, scratch, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            s = Status::IOError("Failed to read from file: " + filename_);
            break;
        }
        *result = Slice(scratch, static_cast<size_t>(r));
        break;
    }
    return s;
}

Status PosixSequentialFile::Skip(uint64_t n) {
    if (lseek(fd_, static_cast<off_t>(n), SEEK_CUR) == static_cast<off_t>(-1)) {
        return Status::IOError("Failed to lseek in file: " + filename_);
    }
    return Status::OK();
}

WALReader::WALReader(std::unique_ptr<SequentialFile> file, Reporter* reporter, bool checksum)
    : file_(std::move(file)), reporter_(reporter), checksum_(checksum) {}

void WALReader::ReportCorruption(size_t bytes, const std::string& reason) {
    ReportDrop(bytes, Status::Corruption(reason));
}

void WALReader::ReportDrop(size_t bytes, const Status& reason) {
    if (reporter_ != nullptr) {
        reporter_->Corruption(bytes, reason);
    }
}

unsigned int WALReader::ReadPhysicalRecord(Slice* result, Status* status) {
    while (true) {
        if (buffer_.size() < wal::kHeaderSize) {
            if (!eof_) {
                // Read next block
                buffer_.clear();
                Status s = file_->Read(wal::kBlockSize, &buffer_, backing_store_);
                end_of_buffer_offset_ += buffer_.size();
                if (!s.ok()) {
                    buffer_.clear();
                    ReportDrop(wal::kBlockSize, s);
                    eof_ = true;
                    if (status) *status = s;
                    return wal::kZeroType;
                } else if (buffer_.size() < wal::kBlockSize) {
                    eof_ = true;
                }
                continue;
            } else {
                buffer_.clear();
                if (status) *status = Status::OK();
                return wal::kZeroType; // EOF reached
            }
        }

        // Parse 7-byte header
        const char* header = buffer_.data();
        const uint32_t a = static_cast<uint8_t>(header[4]);
        const uint32_t b = static_cast<uint8_t>(header[5]);
        const unsigned int type = static_cast<uint8_t>(header[6]);
        const uint32_t length = a | (b << 8);

        if (wal::kHeaderSize + length > buffer_.size()) {
            size_t drop_size = buffer_.size();
            buffer_.clear();
            if (!eof_) {
                ReportCorruption(drop_size, "Bad record length exceeds block size");
                if (status) *status = Status::Corruption("Bad record length exceeds block size");
                return wal::kZeroType;
            }
            if (status) *status = Status::OK();
            return wal::kZeroType;
        }

        if (type == wal::kZeroType && length == 0) {
            // Skip zero padding at the end of block
            buffer_.clear();
            continue;
        }

        // Checksum verification
        if (checksum_) {
            uint32_t expected_crc = wal::CRC32C::Unmask(
                static_cast<uint32_t>(static_cast<uint8_t>(header[0])) |
                (static_cast<uint32_t>(static_cast<uint8_t>(header[1])) << 8) |
                (static_cast<uint32_t>(static_cast<uint8_t>(header[2])) << 16) |
                (static_cast<uint32_t>(static_cast<uint8_t>(header[3])) << 24)
            );
            char type_byte = static_cast<char>(type);
            uint32_t actual_crc = wal::CRC32C::Value(&type_byte, 1);
            actual_crc = wal::CRC32C::Extend(actual_crc, header + wal::kHeaderSize, length);
            if (actual_crc != expected_crc) {
                size_t drop_size = buffer_.size();
                buffer_.clear();
                ReportCorruption(drop_size, "CRC32C checksum mismatch");
                if (status) *status = Status::Corruption("CRC32C checksum mismatch");
                return wal::kZeroType;
            }
        }

        buffer_.remove_prefix(wal::kHeaderSize + length);
        *result = Slice(header + wal::kHeaderSize, length);
        if (status) *status = Status::OK();
        return type;
    }
}

bool WALReader::ReadRecord(Slice* record, std::string* scratch, Status* status) {
    scratch->clear();
    record->clear();
    bool in_fragmented_record = false;
    uint64_t prospective_record_offset = 0;

    Slice fragment;
    while (true) {
        Status s = Status::OK();
        const unsigned int record_type = ReadPhysicalRecord(&fragment, &s);
        if (!s.ok()) {
            if (status) *status = s;
            return false;
        }

        uint64_t physical_record_offset = end_of_buffer_offset_ - buffer_.size() - wal::kHeaderSize - fragment.size();

        switch (record_type) {
            case wal::kFullType:
                if (in_fragmented_record) {
                    ReportCorruption(scratch->size(), "Partial record without end chunk");
                }
                prospective_record_offset = physical_record_offset;
                scratch->clear();
                *record = fragment;
                last_record_offset_ = prospective_record_offset;
                if (status) *status = Status::OK();
                return true;

            case wal::kFirstType:
                if (in_fragmented_record) {
                    ReportCorruption(scratch->size(), "Partial record without end chunk");
                }
                prospective_record_offset = physical_record_offset;
                scratch->assign(fragment.data(), fragment.size());
                in_fragmented_record = true;
                break;

            case wal::kMiddleType:
                if (!in_fragmented_record) {
                    ReportCorruption(fragment.size(), "Missing start of fragmented record");
                } else {
                    scratch->append(fragment.data(), fragment.size());
                }
                break;

            case wal::kLastType:
                if (!in_fragmented_record) {
                    ReportCorruption(fragment.size(), "Missing start of fragmented record");
                } else {
                    scratch->append(fragment.data(), fragment.size());
                    *record = Slice(*scratch);
                    last_record_offset_ = prospective_record_offset;
                    if (status) *status = Status::OK();
                    return true;
                }
                break;

            case wal::kZeroType:
                if (in_fragmented_record) {
                    ReportCorruption(scratch->size(), "Partial record at end of file");
                    if (status) *status = Status::Corruption("Partial record at end of file");
                    return false;
                }
                if (status) *status = Status::OK();
                return false; // EOF reached cleanly

            default: {
                ReportCorruption(fragment.size(), "Unknown record type: " + std::to_string(record_type));
                in_fragmented_record = false;
                scratch->clear();
                if (status) *status = Status::Corruption("Unknown record type: " + std::to_string(record_type));
                return false;
            }
        }
    }
}

} // namespace lsm
