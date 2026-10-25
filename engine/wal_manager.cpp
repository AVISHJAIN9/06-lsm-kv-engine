#include "wal_manager.hpp"
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>

namespace lsm {

namespace fs = std::filesystem;

WALManager::WALManager(WALOptions options)
    : options_(std::move(options)), current_log_number_(1) {}

WALManager::~WALManager() {
    Close();
}

std::string WALManager::GetLogFilePath(uint64_t log_num) const {
    std::ostringstream oss;
    oss << options_.wal_dir << "/" << std::setfill('0') << std::setw(6) << log_num << ".log";
    return oss.str();
}

Status WALManager::OpenNewLogWriterLocked(uint64_t log_num) {
    std::string path = GetLogFilePath(log_num);

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return Status::IOError("Failed to open WAL file: " + path);
    }

    auto writable_file = std::make_unique<PosixWritableFile>(path, fd);
    current_writer_ = std::make_unique<WALWriter>(std::move(writable_file), log_num);
    active_logs_.push_back(log_num);
    return Status::OK();
}

Status WALManager::Open() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Create WAL directory if missing
    std::error_code ec;
    fs::create_directories(options_.wal_dir, ec);
    if (ec) {
        return Status::IOError("Failed to create WAL directory: " + options_.wal_dir + " (" + ec.message() + ")");
    }

    // Discover existing logs to determine next log number
    uint64_t max_found = 0;
    active_logs_.clear();

    for (const auto& entry : fs::directory_iterator(options_.wal_dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".log") {
            std::string stem = entry.path().stem().string();
            try {
                uint64_t num = std::stoull(stem);
                active_logs_.push_back(num);
                if (num > max_found) {
                    max_found = num;
                }
            } catch (...) {}
        }
    }

    std::sort(active_logs_.begin(), active_logs_.end());

    current_log_number_ = (max_found > 0) ? (max_found + 1) : 1;
    return OpenNewLogWriterLocked(current_log_number_);
}

Status WALManager::Rotate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_writer_) {
        if (options_.sync_on_rotate) {
            current_writer_->Sync();
        }
        current_writer_->Close();
        current_writer_.reset();
    }

    current_log_number_++;
    Status s = OpenNewLogWriterLocked(current_log_number_);
    if (s.ok()) {
        MetricsCollector::Instance().AddCounter(CounterType::kWalRotations, 1);
    }
    return s;
}

Status WALManager::Append(const Slice& record, bool sync) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!current_writer_) {
        Status s = OpenNewLogWriterLocked(current_log_number_);
        if (!s.ok()) return s;
    }

    // Check if rotation threshold reached
    if (current_writer_->GetFileSize() >= options_.max_wal_file_size) {
        if (options_.sync_on_rotate) {
            current_writer_->Sync();
        }
        current_writer_->Close();
        current_writer_.reset();

        current_log_number_++;
        Status s = OpenNewLogWriterLocked(current_log_number_);
        if (!s.ok()) return s;
        MetricsCollector::Instance().AddCounter(CounterType::kWalRotations, 1);
    }

    uint64_t before_size = current_writer_->GetFileSize();
    Status s = current_writer_->AddRecord(record);
    if (!s.ok()) return s;

    uint64_t written_bytes = current_writer_->GetFileSize() - before_size;
    MetricsCollector::Instance().AddCounter(CounterType::kBytesWritten, written_bytes);
    MetricsCollector::Instance().AddCounter(CounterType::kWalBytesFlushed, written_bytes);

    if (sync) {
        s = current_writer_->Sync();
        if (s.ok()) {
            MetricsCollector::Instance().AddCounter(CounterType::kWalSyncCount, 1);
        }
    }

    return s;
}

Status WALManager::Sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_writer_) {
        Status s = current_writer_->Sync();
        if (s.ok()) {
            MetricsCollector::Instance().AddCounter(CounterType::kWalSyncCount, 1);
        }
        return s;
    }
    return Status::OK();
}

Status WALManager::PurgeObsoleteWALs(uint64_t min_active_log_num, size_t* deleted_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;

    std::vector<uint64_t> remaining_logs;
    for (uint64_t log_num : active_logs_) {
        if (log_num < min_active_log_num && log_num != current_log_number_) {
            std::string path = GetLogFilePath(log_num);
            std::error_code ec;
            fs::remove(path, ec);
            if (!ec) {
                count++;
            }
        } else {
            remaining_logs.push_back(log_num);
        }
    }

    active_logs_ = std::move(remaining_logs);
    if (deleted_count) {
        *deleted_count = count;
    }
    return Status::OK();
}

uint64_t WALManager::GetCurrentLogNumber() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_log_number_;
}

uint64_t WALManager::GetCurrentFileSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_writer_ ? current_writer_->GetFileSize() : 0;
}

uint64_t WALManager::GetTotalWALBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = 0;
    std::error_code ec;
    for (uint64_t log_num : active_logs_) {
        std::string path = GetLogFilePath(log_num);
        if (fs::exists(path, ec)) {
            total += fs::file_size(path, ec);
        }
    }
    return total;
}

std::vector<uint64_t> WALManager::GetActiveLogNumbers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_logs_;
}

std::vector<std::string> WALManager::GetActiveLogFiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> files;
    files.reserve(active_logs_.size());
    for (uint64_t log_num : active_logs_) {
        files.push_back(GetLogFilePath(log_num));
    }
    return files;
}

Status WALManager::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_writer_) {
        Status s = current_writer_->Close();
        current_writer_.reset();
        return s;
    }
    return Status::OK();
}

} // namespace lsm
