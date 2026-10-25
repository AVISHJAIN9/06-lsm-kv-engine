#pragma once

#include <string>
#include <vector>
#include <set>
#include <utility>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "internal_key.hpp"
#include "leveled_compaction.hpp"

namespace lsm {

// Tag definitions for VersionEdit binary serialization protocol (Feature 92)
enum class Tag : uint32_t {
    kComparator      = 1,
    kLogNumber        = 2,
    kNextFileNumber   = 3,
    kLastSequence     = 4,
    kCompactPointer   = 5,
    kDeletedFile      = 6,
    kNewFile          = 7,
    kMaxTag           = 8
};

// VersionEdit: Encodes atomic delta modifications applied to the database state (Feature 92).
class VersionEdit {
public:
    VersionEdit() = default;
    ~VersionEdit() = default;

    void Clear();

    void SetComparatorName(const Slice& name) {
        has_comparator_ = true;
        comparator_ = name.ToString();
    }

    void SetLogNumber(uint64_t num) {
        has_log_number_ = true;
        log_number_ = num;
    }

    void SetNextFile(uint64_t num) {
        has_next_file_number_ = true;
        next_file_number_ = num;
    }

    void SetLastSequence(SequenceNumber seq) {
        has_last_sequence_ = true;
        last_sequence_ = seq;
    }

    void SetCompactPointer(int level, const InternalKey& key) {
        compact_pointers_.push_back({level, key});
    }

    // Adds a newly produced SSTable file to level
    void AddFile(int level, uint64_t file_number, uint64_t file_size,
                 const InternalKey& smallest, const InternalKey& largest) {
        FileMetaData f;
        f.file_number = file_number;
        f.file_size = file_size;
        f.smallest_key = smallest.Encode().ToString();
        f.largest_key = largest.Encode().ToString();
        new_files_.push_back({level, std::move(f)});
    }

    void AddFile(int level, const FileMetaData& f) {
        new_files_.push_back({level, f});
    }

    // Deletes an obsolete SSTable file from level
    void DeleteFile(int level, uint64_t file_number) {
        deleted_files_.insert({level, file_number});
    }

    // Serialization / Deserialization
    void EncodeTo(std::string* dst) const;
    Status DecodeFrom(const Slice& src);

    std::string DebugString() const;

    // Field accessors
    bool has_comparator() const { return has_comparator_; }
    const std::string& comparator() const { return comparator_; }

    bool has_log_number() const { return has_log_number_; }
    uint64_t log_number() const { return log_number_; }

    bool has_next_file_number() const { return has_next_file_number_; }
    uint64_t next_file_number() const { return next_file_number_; }

    bool has_last_sequence() const { return has_last_sequence_; }
    SequenceNumber last_sequence() const { return last_sequence_; }

    const std::vector<std::pair<int, InternalKey>>& compact_pointers() const {
        return compact_pointers_;
    }

    const std::set<std::pair<int, uint64_t>>& deleted_files() const {
        return deleted_files_;
    }

    const std::vector<std::pair<int, FileMetaData>>& new_files() const {
        return new_files_;
    }

private:
    friend class VersionSet;
    friend class VersionBuilder;

    bool has_comparator_{false};
    std::string comparator_{};

    bool has_log_number_{false};
    uint64_t log_number_{0};

    bool has_next_file_number_{false};
    uint64_t next_file_number_{0};

    bool has_last_sequence_{false};
    SequenceNumber last_sequence_{0};

    std::vector<std::pair<int, InternalKey>> compact_pointers_{};
    std::set<std::pair<int, uint64_t>> deleted_files_{};
    std::vector<std::pair<int, FileMetaData>> new_files_{};
};

} // namespace lsm
