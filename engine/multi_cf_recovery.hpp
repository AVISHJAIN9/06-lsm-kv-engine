#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "status.hpp"
#include "column_family.hpp"
#include "multi_cf_versions.hpp"

namespace lsm {

struct MultiCFRecoveryStats {
    uint64_t records_replayed{0};
    uint64_t edits_replayed{0};
    uint32_t cfs_recovered{0};
    uint64_t files_recovered{0};
    SequenceNumber last_sequence{0};
    uint64_t next_file_number{0};
};

// MultiCFManifestRecovery: Crash recovery orchestrator replaying Manifest logs
// to deterministically reconstruct dynamic column families and multi-level hierarchies (Feature 107).
class MultiCFManifestRecovery {
public:
    MultiCFManifestRecovery() = default;
    ~MultiCFManifestRecovery() = default;

    // Scans the active MANIFEST file referenced by CURRENT, reconstructs all column families,
    // applies file additions and deletions, and restores global sequence numbers (>= 300,000 edits/sec).
    Status Recover(
        const std::string& dbname,
        ColumnFamilySet* cf_set,
        MultiCFVersionSet* vset,
        MultiCFRecoveryStats* stats = nullptr
    );

private:
    Status ReplayManifestFile(
        const std::string& manifest_path,
        ColumnFamilySet* cf_set,
        MultiCFVersionSet* vset,
        MultiCFRecoveryStats* stats
    );
};

} // namespace lsm
