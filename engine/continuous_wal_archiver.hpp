#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include "status.hpp"

namespace lsm {

struct ArchivedSegmentMeta {
    uint64_t segment_id{0};
    std::string archive_path;
    uint64_t min_seq{0};
    uint64_t max_seq{0};
    int64_t min_timestamp_micros{0};
    int64_t max_timestamp_micros{0};
    uint64_t raw_size{0};
    uint64_t compressed_size{0};
    uint32_t crc32c{0};
    std::string sha256_hex;
};

// ContinuousWALArchiver: Continuous background streaming, compression, and cataloging of rotated WAL segments (Feature 135)
class ContinuousWALArchiver {
public:
    explicit ContinuousWALArchiver(std::string archive_dir);
    ~ContinuousWALArchiver() = default;

    ContinuousWALArchiver(const ContinuousWALArchiver&) = delete;
    ContinuousWALArchiver& operator=(const ContinuousWALArchiver&) = delete;

    // Compresses and archives a rotated WAL segment with SHA-256 and CRC32C verification
    Status ArchiveSegment(
        uint64_t segment_id,
        const std::string& raw_wal_path,
        uint64_t min_seq,
        uint64_t max_seq,
        int64_t min_ts,
        int64_t max_ts,
        ArchivedSegmentMeta* out_meta = nullptr
    );

    // Restores and decompresses an archived segment to dst_wal_path
    Status RestoreSegment(uint64_t segment_id, const std::string& dst_wal_path);

    // Lists all archived segments
    std::vector<ArchivedSegmentMeta> GetArchivedSegments() const;

    // Finds archived segments overlapping the specified timestamp interval [start_ts, target_ts]
    std::vector<ArchivedSegmentMeta> GetSegmentsCoveringRange(int64_t start_ts, int64_t target_ts) const;

    std::string GetArchiveDir() const { return archive_dir_; }

private:
    std::string archive_dir_;
    mutable std::mutex mutex_;
    std::vector<ArchivedSegmentMeta> segments_;

    void InitializeDirectory();
    Status SaveArchiveIndex();
    Status LoadArchiveIndex();

    static uint32_t ComputeCRC32C(const uint8_t* data, size_t len);
    static std::string ComputeSHA256(const uint8_t* data, size_t len);
};

} // namespace lsm
