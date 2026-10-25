#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"

namespace lsm {

struct ArchiveManifest {
    uint64_t log_number{0};
    uint64_t original_bytes{0};
    uint64_t compressed_bytes{0};
    uint32_t crc32c{0};
    SequenceNumber min_lsn{0};
    SequenceNumber max_lsn{0};
    uint64_t archive_timestamp{0};
    std::string archive_file{};
    std::string algorithm{"LSM-ZPACK"};

    std::string ToJson() const;
};

class ColdTierArchiver {
public:
    explicit ColdTierArchiver(std::string archive_dir);
    ~ColdTierArchiver() = default;

    Status ArchiveSegment(
        uint64_t log_num,
        const std::string& source_wal_path,
        SequenceNumber min_lsn,
        SequenceNumber max_lsn,
        ArchiveManifest* out_manifest = nullptr
    );

    Status RestoreSegment(
        const std::string& archive_path,
        const std::string& restored_wal_path,
        uint32_t expected_crc = 0
    );

    std::vector<ArchiveManifest> ListArchives() const;

    // Fast byte-level compression and decompression engine
    static std::string Compress(const std::string& input);
    static Status Decompress(const std::string& input, std::string* output);

private:
    std::string archive_dir_{};
    mutable std::mutex mutex_{};
    std::vector<ArchiveManifest> manifests_{};
};

} // namespace lsm
