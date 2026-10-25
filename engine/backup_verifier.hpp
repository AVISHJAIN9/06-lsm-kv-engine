#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "status.hpp"

namespace lsm {

struct VerificationReport {
    bool is_valid{true};
    uint32_t files_scanned{0};
    uint64_t bytes_scanned{0};
    std::vector<std::string> corrupted_files;
    std::vector<std::string> error_details;

    std::string ToString() const;
};

// BackupVerifier: Offline cryptographic hash, block trailer, and bit-rot corruption detector (Feature 137)
class BackupVerifier {
public:
    BackupVerifier() = default;
    ~BackupVerifier() = default;

    // Scans a single physical file, computes cryptographic digests and checks against expected values
    Status VerifyFile(
        const std::string& file_path,
        const std::string& expected_sha256,
        uint64_t expected_size = 0
    );

    // Scans an entire backup directory against a manifest hash catalog
    VerificationReport VerifyBackupDirectory(
        const std::string& backup_dir,
        const std::unordered_map<std::string, std::string>& expected_sha256_catalog
    );

    static std::string ComputeFileSHA256(const std::string& file_path);
    static uint32_t ComputeFileCRC32C(const std::string& file_path);
};

} // namespace lsm
