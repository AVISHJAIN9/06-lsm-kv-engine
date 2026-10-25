#include "version.hpp"
#include <sstream>
#include <algorithm>

namespace lsm {

Version::Version(VersionSet* vset) : vset_(vset), next_(this), prev_(this), refs_(0) {}

Version::~Version() {
    // Unlink from circular doubly-linked list
    prev_->next_ = next_;
    next_->prev_ = prev_;
}

VersionSet* Version::vset() const {
    return vset_;
}

void Version::Unref() {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}

size_t Version::TotalFiles() const {
    size_t total = 0;
    for (int l = 0; l < kNumLevels; ++l) {
        total += files_[l].size();
    }
    return total;
}

uint64_t Version::TotalBytes() const {
    uint64_t bytes = 0;
    for (int l = 0; l < kNumLevels; ++l) {
        for (const auto& f : files_[l]) {
            bytes += f.file_size;
        }
    }
    return bytes;
}

std::string Version::DebugString() const {
    std::ostringstream oss;
    oss << "Version (Refs=" << refs_.load() << ", TotalFiles=" << TotalFiles()
        << ", TotalBytes=" << TotalBytes() << ") [";
    for (int l = 0; l < kNumLevels; ++l) {
        if (!files_[l].empty()) {
            oss << " L" << l << ":" << files_[l].size();
        }
    }
    oss << " ]";
    return oss.str();
}

// -----------------------------------------------------------------------------
// VersionBuilder Implementation
// -----------------------------------------------------------------------------
VersionBuilder::VersionBuilder(VersionSet* vset, Version* base)
    : vset_(vset), base_(base) {
    if (base_) {
        base_->Ref();
        for (int l = 0; l < Version::kNumLevels; ++l) {
            compact_pointers_[l] = base_->compact_pointer(l);
        }
    }
}

void VersionBuilder::Apply(const VersionEdit* edit) {
    if (!edit) return;

    // 1. Update compaction pointers
    for (const auto& cp : edit->compact_pointers()) {
        int level = cp.first;
        if (level >= 0 && level < Version::kNumLevels) {
            compact_pointers_[level] = cp.second.Encode().ToString();
        }
    }

    // 2. Record deleted files
    for (const auto& df : edit->deleted_files()) {
        int level = df.first;
        uint64_t file_num = df.second;
        if (level >= 0 && level < Version::kNumLevels) {
            levels_[level].deleted_files.insert(file_num);
        }
    }

    // 3. Record added files
    for (const auto& nf : edit->new_files()) {
        int level = nf.first;
        const FileMetaData& f = nf.second;
        if (level >= 0 && level < Version::kNumLevels) {
            levels_[level].added_files.push_back(f);
        }
    }
}

void VersionBuilder::SaveTo(Version* v) {
    if (!v) return;
    v->vset_ = vset_;

    for (int l = 0; l < Version::kNumLevels; ++l) {
        v->set_compact_pointer(l, compact_pointers_[l]);

        std::vector<FileMetaData> result;
        const auto& deleted = levels_[l].deleted_files;

        // Base files (if base version exists)
        if (base_) {
            for (const auto& f : base_->files(l)) {
                if (deleted.find(f.file_number) == deleted.end()) {
                    result.push_back(f);
                }
            }
        }

        // Added files from VersionEdit deltas
        for (const auto& f : levels_[l].added_files) {
            if (deleted.find(f.file_number) == deleted.end()) {
                result.push_back(f);
            }
        }

        // Sort Level 0 by file number ascending (or descending), and Level 1..N by smallest key
        if (l == 0) {
            std::sort(result.begin(), result.end(), [](const FileMetaData& a, const FileMetaData& b) {
                return a.file_number < b.file_number;
            });
        } else {
            std::sort(result.begin(), result.end(), [](const FileMetaData& a, const FileMetaData& b) {
                return a.smallest_key < b.smallest_key;
            });
        }

        v->files(l) = std::move(result);
    }

    if (base_) {
        base_->Unref();
        base_ = nullptr;
    }
}

} // namespace lsm
