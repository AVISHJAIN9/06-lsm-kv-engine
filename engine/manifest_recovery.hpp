#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "status.hpp"
#include "version_set.hpp"

namespace lsm {

// ManifestRecovery: Startup manifest roll-forward parser and inventory integrity validator (Feature 95).
// Deterministically restores multi-level file inventories, next file numbers, and compaction watermarks.
class ManifestRecovery {
public:
    ManifestRecovery() = default;
    ~ManifestRecovery() = default;

    // Recovers VersionSet by scanning CURRENT file, replaying binary manifest records,
    // and validating level file assignments.
    static Status RecoverAndVerify(
        const std::string& dbname,
        VersionSet* vset,
        size_t* out_edits_applied = nullptr,
        size_t* out_files_verified = nullptr,
        bool verify_sstable_files = false
    );
};

} // namespace lsm
