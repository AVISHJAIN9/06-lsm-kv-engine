#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include "status.hpp"
#include "slice.hpp"

namespace lsm {

struct ArchivalManifest {
    uint64_t segment_id{0};
    uint64_t min_lsn{0};
    uint64_t max_lsn{0};
    uint64_t uncompressed_size{0};
    uint64_t compressed_size{0};
    uint32_t checksum_crc32c{0};
    std::string sha256_digest{""};
    std::string archive_filename{""};
    uint64_t timestamp_utc{0};

    std::string ToJSON() const;
    static Status FromJSON(const std::string& json_str, ArchivalManifest* out_manifest);
};

class WALArchiver {
public:
    explicit WALArchiver(std::string archive_directory);
    ~WALArchiver() = default;

    // Archives a raw WAL segment file: reads bytes, computes SHA-256 and CRC32C,
    // compresses payload using byte-oriented high-speed compression, writes to archive_dir,
    // and produces a JSON manifest file.
    Status ArchiveSegment(
        const std::string& raw_wal_path,
        uint64_t segment_id,
        uint64_t min_lsn,
        uint64_t max_lsn,
        ArchivalManifest* out_manifest = nullptr
    );

    // Restores an archived compressed segment into a raw WAL file
    Status RestoreSegment(
        uint64_t segment_id,
        const std::string& destination_wal_path
    );

    // Validates the cryptographic SHA-256 digest and compression integrity of an archive
    Status VerifyArchive(uint64_t segment_id);

    // Lists all currently available cold-tier archived segments
    std::vector<ArchivalManifest> ListArchives() const;

    // Pure compression and decompression routines
    static void CompressBuffer(const uint8_t* in, size_t in_len, std::string* out);
    static bool DecompressBuffer(const uint8_t* in, size_t in_len, size_t orig_len, std::string* out);

    // SHA-256 hash calculation helper returning lowercase hex string
    static std::string ComputeSHA256Hex(const uint8_t* data, size_t length);

private:
    std::string archive_dir_;
    mutable std::mutex mu_;
    std::map<uint64_t, ArchivalManifest> manifests_;

    void LoadExistingManifests();
};

} // namespace lsm
