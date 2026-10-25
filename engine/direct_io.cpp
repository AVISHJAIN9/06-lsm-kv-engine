#include "direct_io.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace lsm {

void* DirectIOFile::AllocateAlignedBuffer(size_t size, size_t alignment) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
}

void DirectIOFile::FreeAlignedBuffer(void* ptr) {
    if (ptr) {
        std::free(ptr);
    }
}

DirectIOFile::DirectIOFile() : fd_(-1) {}

DirectIOFile::~DirectIOFile() {
    Close();
}

void DirectIOFile::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Status DirectIOFile::Open(const std::string& path, bool create_if_missing, bool direct_io) {
    Close();
    std::lock_guard<std::mutex> lock(mutex_);

    path_ = path;
    direct_io_ = direct_io;

    int flags = O_RDWR;
    if (create_if_missing) {
        flags |= O_CREAT;
    }

#ifdef O_DIRECT
    if (direct_io) {
        flags |= O_DIRECT;
    }
#endif

    fd_ = ::open(path.c_str(), flags, 0644);
    if (fd_ < 0) {
        return Status::IOError("Failed to open file: " + path);
    }

#ifdef __APPLE__
    if (direct_io) {
        // macOS Direct I/O bypass equivalent: F_NOCACHE turns off OS buffer caching
        ::fcntl(fd_, F_NOCACHE, 1);
    }
#endif

    return Status::OK();
}

Status DirectIOFile::WriteAligned(uint64_t offset, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        return Status::InvalidArgument("DirectIOFile is not open");
    }

    if (size == 0) return Status::OK();

    // Direct I/O requires aligned offset, size, and memory buffer
    uint64_t aligned_offset = (offset / kPageSize) * kPageSize;
    size_t offset_within_page = offset - aligned_offset;
    size_t total_aligned_size = ((offset_within_page + size + kPageSize - 1) / kPageSize) * kPageSize;

    void* aligned_buf = AllocateAlignedBuffer(total_aligned_size);
    if (!aligned_buf) {
        return Status::IOError("Failed to allocate 4KB aligned buffer for Direct I/O write");
    }

    // Read existing page if write starts in the middle of a block
    if (offset_within_page > 0 || total_aligned_size > size) {
        ssize_t r = ::pread(fd_, aligned_buf, total_aligned_size, aligned_offset);
        if (r < 0) {
            std::memset(aligned_buf, 0, total_aligned_size);
        }
    }

    // Copy user payload into aligned buffer at correct offset
    std::memcpy(static_cast<char*>(aligned_buf) + offset_within_page, data, size);

    // Perform unbuffered DMA direct write
    ssize_t written = ::pwrite(fd_, aligned_buf, total_aligned_size, aligned_offset);
    FreeAlignedBuffer(aligned_buf);

    if (written < 0 || static_cast<size_t>(written) < total_aligned_size) {
        return Status::IOError("Direct I/O pwrite failed on file: " + path_);
    }

    return Status::OK();
}

Status DirectIOFile::ReadAligned(uint64_t offset, size_t size, void* out_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        return Status::InvalidArgument("DirectIOFile is not open");
    }

    if (size == 0) return Status::OK();

    uint64_t aligned_offset = (offset / kPageSize) * kPageSize;
    size_t offset_within_page = offset - aligned_offset;
    size_t total_aligned_size = ((offset_within_page + size + kPageSize - 1) / kPageSize) * kPageSize;

    void* aligned_buf = AllocateAlignedBuffer(total_aligned_size);
    if (!aligned_buf) {
        return Status::IOError("Failed to allocate 4KB aligned buffer for Direct I/O read");
    }

    ssize_t bytes_read = ::pread(fd_, aligned_buf, total_aligned_size, aligned_offset);
    if (bytes_read < 0) {
        FreeAlignedBuffer(aligned_buf);
        return Status::IOError("Direct I/O pread failed on file: " + path_);
    }

    size_t available = (bytes_read > static_cast<ssize_t>(offset_within_page)) ?
                       (bytes_read - offset_within_page) : 0;
    size_t copy_bytes = std::min(size, available);

    std::memcpy(out_data, static_cast<char*>(aligned_buf) + offset_within_page, copy_bytes);
    FreeAlignedBuffer(aligned_buf);

    if (copy_bytes < size) {
        return Status::Corruption("Direct I/O read reached unexpected EOF");
    }

    return Status::OK();
}

Status DirectIOFile::Sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return Status::InvalidArgument("File not open");
#ifdef __APPLE__
    if (::fcntl(fd_, F_FULLFSYNC) != 0) {
        ::fsync(fd_);
    }
#else
    ::fdatasync(fd_);
#endif
    return Status::OK();
}

uint64_t DirectIOFile::GetFileSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return 0;
    struct stat st;
    if (::fstat(fd_, &st) == 0) {
        return static_cast<uint64_t>(st.st_size);
    }
    return 0;
}

} // namespace lsm
