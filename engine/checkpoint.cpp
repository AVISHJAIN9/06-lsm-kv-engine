#include "checkpoint.hpp"
#include <filesystem>
#include <algorithm>

namespace lsm {

namespace fs = std::filesystem;

void CheckpointManager::RegisterWAL(uint64_t log_num, const std::string& path, SequenceNumber min_lsn) {
    std::lock_guard<std::mutex> lock(mutex_);
    WALMetadata meta;
    meta.log_number = log_num;
    meta.path = path;
    meta.min_lsn = min_lsn;
    meta.max_lsn = min_lsn;
    meta.file_size = 0;
    meta.is_immutable = false;
    wal_map_[log_num] = meta;
}

void CheckpointManager::UpdateWALMaxLSN(uint64_t log_num, SequenceNumber max_lsn, uint64_t file_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = wal_map_.find(log_num);
    if (it != wal_map_.end()) {
        if (max_lsn > it->second.max_lsn) {
            it->second.max_lsn = max_lsn;
        }
        it->second.file_size = file_size;
    }
}

void CheckpointManager::MarkWALImmutable(uint64_t log_num) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = wal_map_.find(log_num);
    if (it != wal_map_.end()) {
        it->second.is_immutable = true;
    }
}

std::vector<WALMetadata> CheckpointManager::GetEligibleWALsForTruncation(SequenceNumber flushed_lsn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WALMetadata> eligible;
    for (const auto& [log_num, meta] : wal_map_) {
        // Safe condition: WAL is immutable and its maximum sequence number is <= flushed LSN
        if (meta.is_immutable && meta.max_lsn <= flushed_lsn) {
            eligible.push_back(meta);
        }
    }
    std::sort(eligible.begin(), eligible.end(), [](const WALMetadata& a, const WALMetadata& b) {
        return a.log_number < b.log_number;
    });
    return eligible;
}

Status CheckpointManager::ExecuteTruncation(SequenceNumber flushed_lsn, std::vector<std::string>* deleted_files) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint64_t> to_delete;

    for (const auto& [log_num, meta] : wal_map_) {
        if (meta.is_immutable && meta.max_lsn <= flushed_lsn) {
            std::error_code ec;
            fs::remove(meta.path, ec);
            if (deleted_files) {
                deleted_files->push_back(meta.path);
            }
            to_delete.push_back(log_num);
        }
    }

    for (uint64_t log_num : to_delete) {
        wal_map_.erase(log_num);
    }

    return Status::OK();
}

bool CheckpointManager::IsWALActive(uint64_t log_num) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wal_map_.find(log_num) != wal_map_.end();
}

std::vector<WALMetadata> CheckpointManager::GetAllWALMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WALMetadata> list;
    for (const auto& [k, v] : wal_map_) {
        list.push_back(v);
    }
    std::sort(list.begin(), list.end(), [](const WALMetadata& a, const WALMetadata& b) {
        return a.log_number < b.log_number;
    });
    return list;
}

SequenceNumber CheckpointManager::GetOldestActiveLSN() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SequenceNumber oldest = UINT64_MAX;
    for (const auto& [k, v] : wal_map_) {
        if (v.min_lsn < oldest) {
            oldest = v.min_lsn;
        }
    }
    return (oldest == UINT64_MAX) ? 0 : oldest;
}

} // namespace lsm
