#pragma once

#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <fstream>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "internal_key.hpp"
#include "leveled_compaction.hpp"
#include "column_family.hpp"

namespace lsm {

enum class MultiCFTag : uint32_t {
    kAddColumnFamily   = 1,
    kDropColumnFamily  = 2,
    kAddFile           = 3,
    kDeleteFile        = 4,
    kLastSequence      = 5,
    kLogNumber         = 6,
    kNextFileNumber    = 7,
    kMaxTag            = 8
};

struct CFFileAddition {
    uint32_t cf_id{0};
    int level{0};
    FileMetaData file_meta{};
};

struct CFFileDeletion {
    uint32_t cf_id{0};
    int level{0};
    uint64_t file_number{0};
};

struct CFAddEvent {
    uint32_t cf_id{0};
    std::string cf_name{};
};

struct CFDropEvent {
    uint32_t cf_id{0};
};

// MultiCFVersionEdit: Encodes atomic metadata mutations across multiple column families (Feature 106).
class MultiCFVersionEdit {
public:
    MultiCFVersionEdit() = default;
    ~MultiCFVersionEdit() = default;

    void Clear();

    void AddColumnFamily(uint32_t cf_id, const std::string& name);
    void DropColumnFamily(uint32_t cf_id);
    void AddFile(uint32_t cf_id, int level, const FileMetaData& meta);
    void DeleteFile(uint32_t cf_id, int level, uint64_t file_number);

    void SetLastSequence(SequenceNumber seq) {
        has_last_sequence_ = true;
        last_sequence_ = seq;
    }

    void SetLogNumber(uint64_t log_num) {
        has_log_number_ = true;
        log_number_ = log_num;
    }

    void SetNextFileNumber(uint64_t next_file) {
        has_next_file_number_ = true;
        next_file_number_ = next_file;
    }

    // Binary Serialization / Deserialization
    void EncodeTo(std::string* dst) const;
    Status DecodeFrom(const Slice& src);

    const std::vector<CFAddEvent>& added_cfs() const { return added_cfs_; }
    const std::vector<CFDropEvent>& dropped_cfs() const { return dropped_cfs_; }
    const std::vector<CFFileAddition>& new_files() const { return new_files_; }
    const std::vector<CFFileDeletion>& deleted_files() const { return deleted_files_; }

    bool has_last_sequence() const { return has_last_sequence_; }
    SequenceNumber last_sequence() const { return last_sequence_; }

    bool has_log_number() const { return has_log_number_; }
    uint64_t log_number() const { return log_number_; }

    bool has_next_file_number() const { return has_next_file_number_; }
    uint64_t next_file_number() const { return next_file_number_; }

private:
    std::vector<CFAddEvent> added_cfs_{};
    std::vector<CFDropEvent> dropped_cfs_{};
    std::vector<CFFileAddition> new_files_{};
    std::vector<CFFileDeletion> deleted_files_{};

    bool has_last_sequence_{false};
    SequenceNumber last_sequence_{0};

    bool has_log_number_{false};
    uint64_t log_number_{0};

    bool has_next_file_number_{false};
    uint64_t next_file_number_{0};
};

// MultiCFVersionSet: Coordinates atomic multi-CF Manifest logging and version state transitions (Feature 106).
class MultiCFVersionSet {
public:
    explicit MultiCFVersionSet(const std::string& dbname);
    ~MultiCFVersionSet();

    MultiCFVersionSet(const MultiCFVersionSet&) = delete;
    MultiCFVersionSet& operator=(const MultiCFVersionSet&) = delete;

    // Atomically appends batch of multi-CF edits to MANIFEST and updates all CFD states.
    // Guarantees all-or-nothing atomicity.
    Status LogAndApply(
        const std::vector<MultiCFVersionEdit>& edits,
        ColumnFamilySet* cf_set,
        std::mutex* mu = nullptr
    );

    uint64_t NewFileNumber() { return next_file_number_++; }
    uint64_t NextFileNumber() const { return next_file_number_; }
    void SetNextFileNumber(uint64_t n) { next_file_number_ = n; }

    uint64_t ManifestFileNumber() const { return manifest_file_number_; }
    SequenceNumber LastSequence() const { return last_sequence_; }
    void SetLastSequence(SequenceNumber s) { last_sequence_ = s; }

    const std::string& dbname() const { return dbname_; }

    // Creates new MANIFEST file and updates CURRENT pointer
    Status CreateNewManifest(uint64_t manifest_num);

private:
    std::string dbname_{};
    uint64_t next_file_number_{2};
    uint64_t manifest_file_number_{1};
    SequenceNumber last_sequence_{0};

    std::ofstream manifest_file_{};
    mutable std::mutex manifest_mutex_{};

    Status AppendRecordToManifest(const std::string& record);
};

} // namespace lsm
