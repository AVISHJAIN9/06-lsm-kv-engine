#include "fault_env.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <cerrno>

namespace lsm {

void FaultInjector::SetConfig(const FaultConfig& config) {
    std::lock_guard<std::mutex> lock(mu_);
    config_ = config;
    enabled_.store(true, std::memory_order_release);
}

void FaultInjector::Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    config_ = FaultConfig{};
    total_bytes_written_.store(0, std::memory_order_relaxed);
    injected_count_.store(0, std::memory_order_relaxed);
    enabled_.store(false, std::memory_order_release);
}

Status FaultInjector::CheckAndTrackWrite(
    size_t incoming_bytes,
    std::string* inout_payload,
    size_t* out_written_bytes
) {
    if (!IsEnabled()) {
        if (out_written_bytes) *out_written_bytes = incoming_bytes;
        return Status::OK();
    }

    std::lock_guard<std::mutex> lock(mu_);

    if (config_.type == FaultType::kSimulateDiskFull) {
        injected_count_.fetch_add(1, std::memory_order_relaxed);
        return Status::IOError("Simulated Disk Full: ENOSPC");
    }

    if (config_.type == FaultType::kFailAfterBytes) {
        size_t current = total_bytes_written_.load(std::memory_order_relaxed);
        if (current + incoming_bytes > config_.byte_limit) {
            injected_count_.fetch_add(1, std::memory_order_relaxed);
            return Status::IOError("Simulated Disk Full: Exceeded byte threshold (" + std::to_string(config_.byte_limit) + " bytes)");
        }
    }

    if (config_.type == FaultType::kShortWrite && config_.max_write_chunk > 0) {
        if (incoming_bytes > config_.max_write_chunk) {
            if (out_written_bytes) *out_written_bytes = config_.max_write_chunk;
            total_bytes_written_.fetch_add(config_.max_write_chunk, std::memory_order_relaxed);
            injected_count_.fetch_add(1, std::memory_order_relaxed);
            return Status::OK();
        }
    }

    if (config_.type == FaultType::kCorruptPayload && inout_payload && !inout_payload->empty()) {
        if (config_.corrupt_offset < inout_payload->size()) {
            (*inout_payload)[config_.corrupt_offset] ^= config_.corrupt_xor_mask;
            injected_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    total_bytes_written_.fetch_add(incoming_bytes, std::memory_order_relaxed);
    if (out_written_bytes) *out_written_bytes = incoming_bytes;
    return Status::OK();
}

Status FaultInjector::CheckAndTrackSync() {
    if (!IsEnabled()) {
        return Status::OK();
    }

    std::lock_guard<std::mutex> lock(mu_);

    if (config_.type == FaultType::kFailOnSync) {
        injected_count_.fetch_add(1, std::memory_order_relaxed);
        return Status::IOError("Simulated fsync failure: EIO");
    }

    if (config_.type == FaultType::kDelaySync && config_.sync_delay_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(config_.sync_delay_us));
    }

    return Status::OK();
}

FaultInjectionWritableFile::FaultInjectionWritableFile(std::string filename, int fd)
    : filename_(std::move(filename)), fd_(fd), file_size_(0) {
    if (fd_ >= 0) {
        struct stat s;
        if (fstat(fd_, &s) == 0) {
            file_size_ = static_cast<uint64_t>(s.st_size);
        }
    }
}

FaultInjectionWritableFile::~FaultInjectionWritableFile() {
    if (fd_ >= 0) {
        Close();
    }
}

Status FaultInjectionWritableFile::Append(const Slice& data) {
    std::string payload(data.data(), data.size());
    size_t allowed_bytes = data.size();

    Status s = FaultInjector::GetInstance().CheckAndTrackWrite(data.size(), &payload, &allowed_bytes);
    if (!s.ok()) {
        return s;
    }

    const char* p = payload.data();
    size_t left = allowed_bytes;

    while (left > 0) {
        ssize_t written = ::write(fd_, p, left);
        if (written < 0) {
            if (errno == EINTR) continue;
            return Status::IOError("Failed to write to file: " + filename_ + ": " + strerror(errno));
        }
        p += written;
        left -= written;
        file_size_ += written;
    }

    if (allowed_bytes < data.size()) {
        return Status::IOError("Short write injected on file: " + filename_);
    }

    return Status::OK();
}

Status FaultInjectionWritableFile::Flush() {
    return Status::OK();
}

Status FaultInjectionWritableFile::Sync() {
    Status s = FaultInjector::GetInstance().CheckAndTrackSync();
    if (!s.ok()) {
        return s;
    }

    if (fd_ >= 0) {
        if (::fsync(fd_) < 0) {
            return Status::IOError("fsync failed for: " + filename_ + ": " + strerror(errno));
        }
    }
    return Status::OK();
}

Status FaultInjectionWritableFile::Close() {
    if (fd_ >= 0) {
        if (::close(fd_) < 0) {
            return Status::IOError("Failed to close file: " + filename_ + ": " + strerror(errno));
        }
        fd_ = -1;
    }
    return Status::OK();
}

} // namespace lsm
