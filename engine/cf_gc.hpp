#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include "status.hpp"
#include "column_family.hpp"

namespace lsm {

// DroppedCFEntry: Tracks pending garbage collection tasks for a dropped column family
struct DroppedCFEntry {
    std::string dbname;
    std::shared_ptr<ColumnFamilyData> cfd;
    std::vector<uint64_t> sstable_files;
};

// ColumnFamilyGC: Reference-counted scavenger for dropped column families (Feature 102).
// Ensures active readers and iterators complete safely before unlinking underlying SSTables.
class ColumnFamilyGC {
public:
    ColumnFamilyGC() = default;
    ~ColumnFamilyGC();

    // Registers a dropped column family and its associated SSTable files for scavenging
    void RegisterDroppedCF(
        const std::string& dbname,
        std::shared_ptr<ColumnFamilyData> cfd,
        std::vector<uint64_t> sstable_files
    );

    // Scans pending dropped CFs, unlinks physical SSTables if reader ref count == 0,
    // and purges memory structures. Returns number of reclaimed CFs.
    size_t Scavenge();

    size_t PendingCount() const;

private:
    mutable std::mutex mutex_{};
    std::vector<DroppedCFEntry> pending_entries_{};
};

} // namespace lsm
