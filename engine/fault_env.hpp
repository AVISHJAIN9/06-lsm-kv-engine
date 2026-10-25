#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include "status.hpp"
#include "slice.hpp"
#include "wal_writer.hpp"

namespace lsm {

enum class FaultType : uint8_t {
    kNone = 0,
    kFailAfterBytes = 1,
    kFailOnSync = 2,
    kCorruptPayload = 3,
    kDelaySync = 4,
    kSimulateDiskFull = 5,
    kShortWrite = 6
};

struct FaultConfig {
    FaultType type{FaultType::kNone};
    size_t byte_limit{0};
    uint64_t sync_delay_us{0};
    size_t corrupt_offset{0};
    uint8_t corrupt_xor_mask{0xFF};
    size_t max_write_chunk{0};
};

class FaultInjector {
public:
    static FaultInjector& GetInstance() {
        static FaultInjector instance;
        return instance;
    }

    void SetConfig(const FaultConfig& config);
    void Reset();
    void Enable(bool enable = true) { enabled_.store(enable, std::memory_order_release); }
    bool IsEnabled() const { return enabled_.load(std::memory_order_acquire); }

    // Interception triggers
    Status CheckAndTrackWrite(size_t incoming_bytes, std::string* inout_payload, size_t* out_written_bytes);
    Status CheckAndTrackSync();

    uint64_t GetInjectedFaultCount() const { return injected_count_.load(std::memory_order_relaxed); }
    size_t GetTotalBytesWritten() const { return total_bytes_written_.load(std::memory_order_relaxed); }

private:
    FaultInjector() = default;
    ~FaultInjector() = default;

    std::atomic<bool> enabled_{false};
    mutable std::mutex mu_{};
    FaultConfig config_{};
    std::atomic<size_t> total_bytes_written_{0};
    std::atomic<uint64_t> injected_count_{0};
};

class FaultInjectionWritableFile : public WritableFile {
public:
    FaultInjectionWritableFile(std::string filename, int fd);
    ~FaultInjectionWritableFile() override;

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
};

} // namespace lsm
