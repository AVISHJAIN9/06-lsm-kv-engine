#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "wal_writer.hpp"
#include "metrics.hpp"

namespace lsm {

struct WALOptions {
    std::string wal_dir{};
    uint64_t max_wal_file_size{64 * 1024 * 1024}; // Default 64MB
    bool sync_on_rotate{true};
};

class WALManager {
public:
    explicit WALManager(WALOptions options);
    ~WALManager();

    WALManager(const WALManager&) = delete;
    WALManager& operator=(const WALManager&) = delete;

    Status Open();
    Status Append(const Slice& record, bool sync = false);
    Status Sync();
    Status Rotate();
    Status PurgeObsoleteWALs(uint64_t min_active_log_num, size_t* deleted_count = nullptr);

    uint64_t GetCurrentLogNumber() const;
    uint64_t GetCurrentFileSize() const;
    uint64_t GetTotalWALBytes() const;
    std::vector<uint64_t> GetActiveLogNumbers() const;
    std::vector<std::string> GetActiveLogFiles() const;
    std::string GetLogFilePath(uint64_t log_num) const;

    Status Close();

private:
    WALOptions options_{};
    mutable std::mutex mutex_{};
    uint64_t current_log_number_{1};
    std::unique_ptr<WALWriter> current_writer_{nullptr};
    std::vector<uint64_t> active_logs_{};

    Status OpenNewLogWriterLocked(uint64_t log_num);
};

} // namespace lsm
