#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <cstddef>
#include "status.hpp"

namespace lsm {

// DirectIOFile: 4KB page-aligned unbuffered Direct I/O bypass controller (Feature 144)
class DirectIOFile {
public:
    static constexpr size_t kPageSize = 4096;

    DirectIOFile();
    ~DirectIOFile();

    DirectIOFile(const DirectIOFile&) = delete;
    DirectIOFile& operator=(const DirectIOFile&) = delete;

    // Opens file with direct I/O cache bypass (O_DIRECT on Linux, F_NOCACHE on macOS)
    Status Open(const std::string& path, bool create_if_missing = true, bool direct_io = true);

    // Closes the file descriptor
    void Close();

    // Writes data with 4KB buffer alignment handling arbitrary offsets
    Status WriteAligned(uint64_t offset, const void* data, size_t size);

    // Reads data with 4KB buffer alignment
    Status ReadAligned(uint64_t offset, size_t size, void* out_data);

    // Flushes all dirty blocks to physical storage
    Status Sync();

    // Returns file size in bytes
    uint64_t GetFileSize() const;

    bool IsOpen() const { return fd_ >= 0; }

    // Allocates memory aligned to 4KB boundary via posix_memalign
    static void* AllocateAlignedBuffer(size_t size, size_t alignment = kPageSize);
    static void FreeAlignedBuffer(void* ptr);

private:
    int fd_{-1};
    std::string path_;
    bool direct_io_{true};
    mutable std::mutex mutex_;
};

} // namespace lsm
