#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"

namespace lsm {

struct WALMetadata {
    uint64_t log_number{0};
    std::string path{};
    SequenceNumber min_lsn{0};
    SequenceNumber max_lsn{0};
    uint64_t file_size{0};
    bool is_immutable{false};
};

class CheckpointManager {
public:
    CheckpointManager() = default;
    ~CheckpointManager() = default;

    CheckpointManager(const CheckpointManager&) = delete;
    CheckpointManager& operator=(const CheckpointManager&) = delete;

    void RegisterWAL(uint64_t log_num, const std::string& path, SequenceNumber min_lsn);
    void UpdateWALMaxLSN(uint64_t log_num, SequenceNumber max_lsn, uint64_t file_size);
    void MarkWALImmutable(uint64_t log_num);

    std::vector<WALMetadata> GetEligibleWALsForTruncation(SequenceNumber flushed_lsn) const;
    Status ExecuteTruncation(SequenceNumber flushed_lsn, std::vector<std::string>* deleted_files = nullptr);

    bool IsWALActive(uint64_t log_num) const;
    std::vector<WALMetadata> GetAllWALMetadata() const;
    SequenceNumber GetOldestActiveLSN() const;

private:
    mutable std::mutex mutex_{};
    std::unordered_map<uint64_t, WALMetadata> wal_map_{};
};

} // namespace lsm
