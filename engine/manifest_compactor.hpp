#pragma once

#include <string>
#include <cstdint>
#include "status.hpp"
#include "version_set.hpp"

namespace lsm {

// ManifestCompactor: Periodically rotates and consolidates the MANIFEST log into a fresh
// baseline snapshot, unlinking obsolete manifests and bounding startup recovery time (Feature 96).
class ManifestCompactor {
public:
    ManifestCompactor() = default;
    ~ManifestCompactor() = default;

    // Compacts active VersionSet state into a new MANIFEST file, atomically updates CURRENT,
    // and garbage-collects the retired manifest.
    static Status RollManifest(
        const std::string& dbname,
        VersionSet* vset,
        uint64_t* out_new_manifest_num = nullptr,
        std::string* out_old_manifest_filename = nullptr
    );
};

} // namespace lsm
