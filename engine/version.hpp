#pragma once

#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <set>
#include <map>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "leveled_compaction.hpp"
#include "version_edit.hpp"

namespace lsm {

class VersionSet;

// Version: Reference-counted snapshot representation of multi-level SSTables (Feature 93).
class Version {
public:
    static constexpr int kNumLevels = 7;

    explicit Version(VersionSet* vset = nullptr);
    ~Version();

    Version(const Version&) = delete;
    Version& operator=(const Version&) = delete;

    void Ref() {
        refs_.fetch_add(1, std::memory_order_relaxed);
    }

    void Unref();

    uint32_t RefCount() const {
        return refs_.load(std::memory_order_relaxed);
    }

    const std::vector<FileMetaData>& files(int level) const {
        return files_[level];
    }

    std::vector<FileMetaData>& files(int level) {
        return files_[level];
    }

    size_t NumFiles(int level) const {
        if (level >= 0 && level < kNumLevels) {
            return files_[level].size();
        }
        return 0;
    }

    size_t TotalFiles() const;
    uint64_t TotalBytes() const;

    const std::string& compact_pointer(int level) const {
        return compact_pointers_[level];
    }

    void set_compact_pointer(int level, std::string ptr) {
        if (level >= 0 && level < kNumLevels) {
            compact_pointers_[level] = std::move(ptr);
        }
    }

    VersionSet* vset() const;

    std::string DebugString() const;

private:
    friend class VersionSet;
    friend class VersionBuilder;

    VersionSet* vset_{nullptr};
    Version* next_{this};
    Version* prev_{this};
    std::atomic<uint32_t> refs_{0};

    std::vector<FileMetaData> files_[kNumLevels];
    std::string compact_pointers_[kNumLevels];
};

// VersionBuilder: Accumulates VersionEdit deltas and produces a new Version (Feature 93).
class VersionBuilder {
public:
    VersionBuilder(VersionSet* vset, Version* base);
    ~VersionBuilder() = default;

    // Applies a delta edit (added/deleted files, compact pointers)
    void Apply(const VersionEdit* edit);

    // Materializes accumulated delta state onto target Version
    void SaveTo(Version* v);

private:
    struct LevelState {
        std::set<uint64_t> deleted_files;
        std::vector<FileMetaData> added_files;
    };

    VersionSet* vset_{nullptr};
    Version* base_{nullptr};
    LevelState levels_[Version::kNumLevels];
    std::string compact_pointers_[Version::kNumLevels];
};

} // namespace lsm
