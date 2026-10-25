#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <fstream>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "version.hpp"
#include "version_edit.hpp"

namespace lsm {

// VersionSet: Manages the collection of active and historical Version representations (Features 91 & 93).
// Coordinates atomic LogAndApply delta commits to MANIFEST log and maintains Version ref-counts.
class VersionSet {
public:
    explicit VersionSet(
        const std::string& dbname,
        const std::string& comparator_name = "lsm.InternalKeyComparator"
    );
    ~VersionSet();

    VersionSet(const VersionSet&) = delete;
    VersionSet& operator=(const VersionSet&) = delete;

    // Applies VersionEdit to current Version, materializes a new Version,
    // appends serialized record to MANIFEST transaction log, fsyncs, and advances current pointer.
    Status LogAndApply(VersionEdit* edit, std::mutex* mu = nullptr);

    // Recovers multi-level LSM state from existing MANIFEST log pointed by CURRENT file.
    Status Recover();

    Version* current() const { return current_; }

    uint64_t NewFileNumber() { return next_file_number_++; }
    void ReuseFileNumber(uint64_t file_number) {
        if (next_file_number_ == file_number + 1) {
            next_file_number_ = file_number;
        }
    }

    uint64_t NextFileNumber() const { return next_file_number_; }
    void SetNextFileNumber(uint64_t num) { next_file_number_ = num; }

    uint64_t ManifestFileNumber() const { return manifest_file_number_; }
    void SetManifestFileNumber(uint64_t num) { manifest_file_number_ = num; }

    SequenceNumber LastSequence() const { return last_sequence_; }
    void SetLastSequence(SequenceNumber s) { last_sequence_ = s; }

    uint64_t LogNumber() const { return log_number_; }
    void SetLogNumber(uint64_t num) { log_number_ = num; }

    const std::string& dbname() const { return dbname_; }
    const std::string& comparator_name() const { return comparator_name_; }

    void AppendVersion(Version* v);

    std::string Summary() const;

private:
    std::string dbname_{};
    std::string comparator_name_{};

    uint64_t next_file_number_{2};
    uint64_t manifest_file_number_{1};
    SequenceNumber last_sequence_{0};
    uint64_t log_number_{0};

    Version dummy_versions_{this}; // Circular doubly-linked sentinel
    Version* current_{nullptr};

    std::ofstream manifest_file_{};
    uint64_t manifest_bytes_written_{0};

    Status WriteSnapshotToManifest(const std::string& manifest_path);
    Status AppendRecordToManifest(const std::string& record);
};

} // namespace lsm
