#include "column_family.hpp"
#include "table_reader.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// ColumnFamilyData Implementation
// -----------------------------------------------------------------------------
ColumnFamilyData::ColumnFamilyData(
    uint32_t id,
    std::string name,
    const ColumnFamilyOptions& options,
    ColumnFamilySet* set
) : id_(id), name_(std::move(name)), options_(options), set_(set),
    mem_(std::make_shared<MemTableCore>()) {}

Status ColumnFamilyData::Put(SequenceNumber seq, const Slice& key, const Slice& value) {
    if (IsDropped()) {
        return Status::InvalidArgument("Column family dropped: " + name_);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!mem_) {
        mem_ = std::make_shared<MemTableCore>();
    }

    if (earliest_mem_seq_.load(std::memory_order_relaxed) == 0) {
        earliest_mem_seq_.store(seq, std::memory_order_release);
    }

    mem_->Add(seq, kTypeValue, key, value);
    return Status::OK();
}

Status ColumnFamilyData::Delete(SequenceNumber seq, const Slice& key) {
    if (IsDropped()) {
        return Status::InvalidArgument("Column family dropped: " + name_);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!mem_) {
        mem_ = std::make_shared<MemTableCore>();
    }

    if (earliest_mem_seq_.load(std::memory_order_relaxed) == 0) {
        earliest_mem_seq_.store(seq, std::memory_order_release);
    }

    mem_->Add(seq, kTypeDeletion, key, Slice(""));
    return Status::OK();
}

Status ColumnFamilyData::Get(const Slice& key, SequenceNumber snapshot_seq, std::string* value) {
    if (IsDropped()) {
        return Status::InvalidArgument("Column family dropped: " + name_);
    }

    std::shared_ptr<MemTableCore> active;
    std::vector<std::shared_ptr<MemTableCore>> imm_list;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        active = mem_;
        imm_list = imm_queue_;
    }

    // 1. Search active memtable
    if (active) {
        Status s;
        if (active->Get(key, snapshot_seq, value, &s)) {
            return s;
        }
    }

    // 2. Search immutable queue (newest to oldest)
    for (auto it = imm_list.rbegin(); it != imm_list.rend(); ++it) {
        if (*it) {
            Status s;
            if ((*it)->Get(key, snapshot_seq, value, &s)) {
                return s;
            }
        }
    }

    // 3. Search Level 0 SSTable files on disk
    std::vector<FileMetaData> l0_files;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l0_files = files_[0];
    }

    for (auto it = l0_files.rbegin(); it != l0_files.rend(); ++it) {
        if (!it->filename.empty()) {
            std::unique_ptr<TableReader> reader;
            TableOptions topts;
            Status s = TableReader::Open(topts, it->filename, it->file_size, nullptr, &reader);
            if (s.ok() && reader) {
                Status gs = reader->Get(key, value);
                if (gs.ok()) {
                    return gs;
                }
            }
        }
    }

    return Status::NotFound("Key not found in column family: " + name_);
}

std::unique_ptr<Iterator> ColumnFamilyData::NewIterator(SequenceNumber /*snapshot_seq*/) {
    if (IsDropped()) {
        return nullptr;
    }

    std::shared_ptr<MemTableCore> active;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active = mem_;
    }

    if (active) {
        return active->NewIterator();
    }
    return nullptr;
}

std::shared_ptr<MemTableCore> ColumnFamilyData::GetActiveMemTable() {
    std::lock_guard<std::mutex> lock(mutex_);
    return mem_;
}

void ColumnFamilyData::RotateMemTable() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mem_ && mem_->Count() > 0) {
        mem_->MarkImmutable();
        imm_queue_.push_back(mem_);
        mem_ = std::make_shared<MemTableCore>();
    }
}

std::vector<std::shared_ptr<MemTableCore>> ColumnFamilyData::GetImmutableQueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    return imm_queue_;
}

void ColumnFamilyData::ClearImmutableQueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    imm_queue_.clear();
}

size_t ColumnFamilyData::TotalMemoryUsage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    if (mem_) total += mem_->ApproximateMemoryUsage();
    for (const auto& imm : imm_queue_) {
        if (imm) total += imm->ApproximateMemoryUsage();
    }
    return total;
}

const std::vector<FileMetaData>& ColumnFamilyData::GetLevelFiles(int level) const {
    static const std::vector<FileMetaData> kEmptyFiles;
    if (level >= 0 && level < Version::kNumLevels) {
        return files_[level];
    }
    return kEmptyFiles;
}

void ColumnFamilyData::AddLevelFile(int level, const FileMetaData& f) {
    if (level >= 0 && level < Version::kNumLevels) {
        files_[level].push_back(f);
    }
}

// -----------------------------------------------------------------------------
// ColumnFamilySet Implementation
// -----------------------------------------------------------------------------
ColumnFamilySet::ColumnFamilySet() {
    // Register default column family (ID = 0)
    ColumnFamilyOptions default_opts;
    default_cfd_ = std::make_shared<ColumnFamilyData>(0, "default", default_opts, this);
    name_to_cfd_["default"] = default_cfd_;
    id_to_cfd_[0] = default_cfd_;
}

std::unique_ptr<ColumnFamilyHandle> ColumnFamilySet::CreateColumnFamily(
    const std::string& name,
    const ColumnFamilyOptions& options
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = name_to_cfd_.find(name);
    if (it != name_to_cfd_.end()) {
        if (!it->second->IsDropped()) {
            return std::make_unique<ColumnFamilyHandle>(it->second);
        }
    }

    uint32_t id = next_cf_id_.fetch_add(1, std::memory_order_acq_rel);
    auto cfd = std::make_shared<ColumnFamilyData>(id, name, options, this);

    name_to_cfd_[name] = cfd;
    id_to_cfd_[id] = cfd;

    return std::make_unique<ColumnFamilyHandle>(cfd);
}

Status ColumnFamilySet::DropColumnFamily(ColumnFamilyHandle* handle) {
    if (!handle || !handle->cfd()) {
        return Status::InvalidArgument("Invalid ColumnFamilyHandle");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ColumnFamilyData* cfd = handle->cfd();
    if (cfd->GetID() == 0) {
        return Status::InvalidArgument("Cannot drop default column family");
    }

    cfd->SetDropped();
    name_to_cfd_.erase(cfd->GetName());
    return Status::OK();
}

std::unique_ptr<ColumnFamilyHandle> ColumnFamilySet::GetDefault() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::make_unique<ColumnFamilyHandle>(default_cfd_);
}

std::unique_ptr<ColumnFamilyHandle> ColumnFamilySet::GetColumnFamily(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = name_to_cfd_.find(name);
    if (it != name_to_cfd_.end() && !it->second->IsDropped()) {
        return std::make_unique<ColumnFamilyHandle>(it->second);
    }
    return nullptr;
}

std::unique_ptr<ColumnFamilyHandle> ColumnFamilySet::GetColumnFamily(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = id_to_cfd_.find(id);
    if (it != id_to_cfd_.end() && !it->second->IsDropped()) {
        return std::make_unique<ColumnFamilyHandle>(it->second);
    }
    return nullptr;
}

std::vector<std::unique_ptr<ColumnFamilyHandle>> ColumnFamilySet::ListColumnFamilies() {
    std::vector<std::unique_ptr<ColumnFamilyHandle>> list;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : name_to_cfd_) {
        if (!kv.second->IsDropped()) {
            list.push_back(std::make_unique<ColumnFamilyHandle>(kv.second));
        }
    }
    return list;
}

std::vector<std::shared_ptr<ColumnFamilyData>> ColumnFamilySet::GetAllCFDs() {
    std::vector<std::shared_ptr<ColumnFamilyData>> list;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : id_to_cfd_) {
        list.push_back(kv.second);
    }
    return list;
}

size_t ColumnFamilySet::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return name_to_cfd_.size();
}

} // namespace lsm
