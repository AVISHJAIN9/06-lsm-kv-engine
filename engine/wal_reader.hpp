#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "wal_format.hpp"

namespace lsm {

class SequentialFile {
public:
    virtual ~SequentialFile() = default;
    virtual Status Read(size_t n, Slice* result, char* scratch) = 0;
    virtual Status Skip(uint64_t n) = 0;
    virtual const std::string& GetFilename() const = 0;
};

class PosixSequentialFile : public SequentialFile {
public:
    PosixSequentialFile(std::string filename, int fd);
    ~PosixSequentialFile() override;

    Status Read(size_t n, Slice* result, char* scratch) override;
    Status Skip(uint64_t n) override;
    const std::string& GetFilename() const override { return filename_; }

private:
    std::string filename_{};
    int fd_{-1};
};

class WALReader {
public:
    class Reporter {
    public:
        virtual ~Reporter() = default;
        virtual void Corruption(size_t bytes, const Status& status) = 0;
    };

    explicit WALReader(std::unique_ptr<SequentialFile> file, Reporter* reporter = nullptr, bool checksum = true);
    ~WALReader() = default;

    WALReader(const WALReader&) = delete;
    WALReader& operator=(const WALReader&) = delete;

    // Reads the next record into *record. Returns true on success, false on EOF / unrecoverable corruption.
    // *status returns the status of read (Status::OK, Status::Corruption, etc.)
    bool ReadRecord(Slice* record, std::string* scratch, Status* status = nullptr);

    uint64_t LastRecordOffset() const { return last_record_offset_; }

private:
    std::unique_ptr<SequentialFile> file_{nullptr};
    Reporter* reporter_{nullptr};
    bool checksum_{true};
    char backing_store_[wal::kBlockSize]{};
    Slice buffer_{};
    bool eof_{false};

    uint64_t last_record_offset_{0};
    uint64_t end_of_buffer_offset_{0};

    // Reads physical chunk from file stream
    unsigned int ReadPhysicalRecord(Slice* result, Status* status);
    void ReportCorruption(size_t bytes, const std::string& reason);
    void ReportDrop(size_t bytes, const Status& reason);
};

} // namespace lsm
