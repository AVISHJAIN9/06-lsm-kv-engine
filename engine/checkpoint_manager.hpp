#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <sys/stat.h>
#include "status.hpp"
#include "slice.hpp"

namespace lsm {

struct CheckpointMetadata {
    uint64_t checkpoint_id{0};
    uint64_t sequence_number{0};
    int64_t timestamp_micros{0};
    std::string checkpoint_dir;
    std::vector<std::string> sstable_files;
    std::vector<std::string> wal_files;
    std::string manifest_file;
    size_t total_bytes{0};
    bool used_hard_links{true};
};

// LiveCheckpointManager: Creates non-blocking online hard-link checkpoints of active LSM database instances (Feature 133)
class LiveCheckpointManager {
public:
    LiveCheckpointManager() = default;
    ~LiveCheckpointManager() = default;

    LiveCheckpointManager(const LiveCheckpointManager&) = delete;
    LiveCheckpointManager& operator=(const LiveCheckpointManager&) = delete;

    // Creates a consistent physical checkpoint in target_dir using atomic POSIX link()
    Status CreateCheckpoint(
        const std::string& db_dir,
        const std::string& checkpoint_dir,
        const std::vector<std::string>& live_sst_files,
        const std::string& manifest_filename,
        const std::vector<std::string>& wal_files,
        uint64_t current_seq,
        CheckpointMetadata* out_meta = nullptr
    );

    // Verifies that SSTables in checkpoint share inodes with source files (confirming zero copy hard links)
    static bool VerifyHardLinks(const std::string& src_file, const std::string& dst_file);
};

} // namespace lsm
