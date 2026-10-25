#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "types.hpp"
#include "status.hpp"
#include "slice.hpp"
#include "memtable_core.hpp"

namespace lsm {

struct PITRLogRecord {
    uint64_t sequence{0};
    int64_t timestamp_micros{0};
    ValueType type{ValueType::kTypeValue};
    std::string key;
    std::string value;

    std::string Serialize() const;
    static bool Deserialize(const std::string& line, PITRLogRecord* out_rec);
};

struct PITRReplaySummary {
    size_t records_scanned{0};
    size_t records_applied{0};
    size_t records_skipped_after_cutoff{0};
    int64_t earliest_timestamp_micros{0};
    int64_t latest_replayed_timestamp_micros{0};
};

// PITRCoordinator: Precision historical Point-in-Time Recovery log replay engine (Feature 136)
class PITRCoordinator {
public:
    PITRCoordinator() = default;
    ~PITRCoordinator() = default;

    // Restores base backup and applies WAL archives up to exact target timestamp
    Status RecoverToTimestamp(
        int64_t target_epoch_micros,
        const std::string& base_backup_dir,
        const std::vector<std::string>& wal_archive_files,
        const std::string& target_restore_dir,
        std::shared_ptr<MemTableCore>& out_memtable,
        PITRReplaySummary* out_summary = nullptr
    );
};

} // namespace lsm
