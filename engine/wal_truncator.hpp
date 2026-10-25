#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include <memory>
#include "status.hpp"
#include "types.hpp"
#include "checkpoint.hpp"

namespace lsm {

struct PruneReport {
    size_t files_examined{0};
    size_t files_pruned{0};
    uint64_t bytes_reclaimed{0};
    std::vector<std::string> pruned_file_paths{};
    uint64_t flushed_lsn{0};
};

class WALTruncator {
public:
    explicit WALTruncator(std::string wal_directory = "");
    ~WALTruncator() = default;

    // Registers a new or existing WAL segment metadata
    void RegisterSegment(const WALMetadata& meta);

    // Updates segment LSN range
    void UpdateSegmentLSN(uint64_t log_num, SequenceNumber min_lsn, SequenceNumber max_lsn);

    // Marks immutable/active
    void SetSegmentImmutable(uint64_t log_num, bool immutable);

    // Scans directory to discover and register existing WAL segments
    Status ScanDirectory(const std::string& wal_dir);

    // Determines eligible segments for pruning where max_lsn <= flushed_lsn and is_immutable
    std::vector<WALMetadata> GetEligibleSegments(SequenceNumber flushed_lsn) const;

    // Prunes (unlinks) all obsolete WAL files where max_lsn <= flushed_lsn
    Status PruneWALs(SequenceNumber flushed_lsn, PruneReport* out_report = nullptr);

    // Inspect registered segments
    std::vector<WALMetadata> GetAllSegments() const;

    // Returns the lowest min_lsn among all currently active/retained segments
    SequenceNumber GetMinActiveLSN() const;

private:
    std::string wal_dir_;
    mutable std::mutex mu_;
    std::map<uint64_t, WALMetadata> segments_;
};

} // namespace lsm
