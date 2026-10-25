#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include "slice.hpp"
#include "status.hpp"
#include "wal_format.hpp"

namespace lsm {

class WritableFile {
public:
    virtual ~WritableFile() = default;
    virtual Status Append(const Slice& data) = 0;
    virtual Status Flush() = 0;
    virtual Status Sync() = 0;
    virtual Status Close() = 0;
    virtual uint64_t GetFileSize() const = 0;
    virtual const std::string& GetFilename() const = 0;
};

class PosixWritableFile : public WritableFile {
public:
    PosixWritableFile(std::string filename, int fd);
    ~PosixWritableFile() override;

    Status Append(const Slice& data) override;
    Status Flush() override;
    Status Sync() override;
    Status Close() override;
    uint64_t GetFileSize() const override { return file_size_; }
    const std::string& GetFilename() const override { return filename_; }

private:
    std::string filename_{};
    int fd_{-1};
    uint64_t file_size_{0};
    char buffer_[65536]{}; // 64KB write buffer for high-throughput appends
    size_t buffer_offset_{0};

    Status FlushBuffer();
};

class WALWriter {
public:
    explicit WALWriter(std::unique_ptr<WritableFile> dest, uint64_t log_number = 0);
    explicit WALWriter(const std::string& filepath, uint64_t log_number = 0);
    ~WALWriter();

    WALWriter(const WALWriter&) = delete;
    WALWriter& operator=(const WALWriter&) = delete;

    Status AddRecord(const Slice& slice);
    Status Flush();
    Status Sync();
    Status Close();

    uint64_t GetLogNumber() const { return log_number_; }
    uint64_t GetFileSize() const { return dest_ ? dest_->GetFileSize() : 0; }
    const std::string& GetFilename() const { static std::string empty; return dest_ ? dest_->GetFilename() : empty; }

private:
    std::unique_ptr<WritableFile> dest_{nullptr};
    uint64_t log_number_{0};
    int block_offset_{0};
    uint32_t type_crc_[wal::kMaxRecordType + 1]{};

    Status EmitPhysicalRecord(wal::RecordType type, const char* ptr, size_t length);
};

} // namespace lsm
