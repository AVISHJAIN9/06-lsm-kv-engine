#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include "status.hpp"

namespace lsm {

struct BackupInfo {
    uint32_t backup_id{0};
    int64_t timestamp_micros{0};
    uint64_t total_size_bytes{0};
    uint64_t number_of_files{0};
    std::vector<std::string> referenced_ssts;
    std::vector<std::string> new_ssts;
    std::string manifest_file;
    bool is_incremental{false};
};

// BackupEngine: Manages deduplicated full and incremental backups with shared SSTable storage (Feature 134)
class BackupEngine {
public:
    explicit BackupEngine(std::string backup_root_dir);
    ~BackupEngine() = default;

    BackupEngine(const BackupEngine&) = delete;
    BackupEngine& operator=(const BackupEngine&) = delete;

    // Creates a new full or incremental backup
    Status CreateNewBackup(
        const std::string& db_dir,
        const std::vector<std::string>& live_sst_files,
        const std::string& manifest_file,
        bool incremental = true,
        BackupInfo* out_info = nullptr
    );

    // Restores a specified backup into target restore_dir
    Status RestoreBackup(uint32_t backup_id, const std::string& restore_dir);

    // Purges older backups keeping the latest keep_count backups and unlinks orphaned shared SSTables
    Status PurgeOldBackups(uint32_t keep_count);

    // Returns info for a specific backup
    Status GetBackupInfo(uint32_t backup_id, BackupInfo* out_info) const;

    // Lists all available backups in chronological order
    std::vector<BackupInfo> ListBackups() const;

    std::string GetSharedSSTDir() const { return shared_sst_dir_; }
    std::string GetBackupRootDir() const { return backup_root_dir_; }

private:
    std::string backup_root_dir_;
    std::string shared_sst_dir_;
    std::string meta_dir_;
    mutable std::mutex mutex_;

    std::vector<BackupInfo> backups_;
    std::unordered_set<std::string> existing_shared_ssts_;
    uint32_t next_backup_id_{1};

    void InitializeDirectories();
    Status SaveManifest();
    Status LoadManifest();
};

} // namespace lsm
