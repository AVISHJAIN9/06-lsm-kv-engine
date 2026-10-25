#include "cf_gc.hpp"
#include <filesystem>
#include <cstdio>

namespace fs = std::filesystem;

namespace lsm {

ColumnFamilyGC::~ColumnFamilyGC() {
    Scavenge();
}

void ColumnFamilyGC::RegisterDroppedCF(
    const std::string& dbname,
    std::shared_ptr<ColumnFamilyData> cfd,
    std::vector<uint64_t> sstable_files
) {
    if (!cfd) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_entries_.push_back({dbname, std::move(cfd), std::move(sstable_files)});
}

size_t ColumnFamilyGC::Scavenge() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t reclaimed = 0;

    auto it = pending_entries_.begin();
    while (it != pending_entries_.end()) {
        // If ref count is 1 (only this GC entry holds a reference) or 0
        if (!it->cfd || it->cfd->RefCount() <= 1) {
            // Unlink associated SSTable files from disk
            for (uint64_t file_num : it->sstable_files) {
                char sst_name[64];
                snprintf(sst_name, sizeof(sst_name), "/%06llu.sst", static_cast<unsigned long long>(file_num));
                std::string path = it->dbname + sst_name;
                std::error_code ec;
                fs::remove(path, ec);
            }

            it = pending_entries_.erase(it);
            reclaimed++;
        } else {
            ++it;
        }
    }

    return reclaimed;
}

size_t ColumnFamilyGC::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_entries_.size();
}

} // namespace lsm
