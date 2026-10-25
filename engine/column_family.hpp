#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "memtable_core.hpp"
#include "leveled_compaction.hpp"
#include "version.hpp"

namespace lsm {

struct ColumnFamilyOptions {
    size_t write_buffer_size{4 * 1024 * 1024}; // 4MB default
    std::string comparator_name{"lsm.InternalKeyComparator"};
    int max_write_buffer_number{2};
    uint64_t target_file_size{2 * 1024 * 1024}; // 2MB SSTables
};

enum class ColumnFamilyStatus {
    kActive,
    kDropping,
    kDropped
};

class ColumnFamilySet;

// ColumnFamilyData: Independent per-CF partition state managing memtables,
// options, level files, sequence watermarks, and drop lifecycle (Features 97 & 98).
class ColumnFamilyData {
public:
    ColumnFamilyData(
        uint32_t id,
        std::string name,
        const ColumnFamilyOptions& options,
        ColumnFamilySet* set = nullptr
    );
    ~ColumnFamilyData() = default;

    ColumnFamilyData(const ColumnFamilyData&) = delete;
    ColumnFamilyData& operator=(const ColumnFamilyData&) = delete;

    uint32_t GetID() const { return id_; }
    const std::string& GetName() const { return name_; }
    const ColumnFamilyOptions& GetOptions() const { return options_; }

    bool IsDropped() const {
        return status_.load(std::memory_order_acquire) == ColumnFamilyStatus::kDropped;
    }

    void SetDropped() {
        status_.store(ColumnFamilyStatus::kDropped, std::memory_order_release);
    }

    void Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
    void Unref() { refs_.fetch_sub(1, std::memory_order_acq_rel); }
    uint32_t RefCount() const { return refs_.load(std::memory_order_relaxed); }

    // MemTable & SSTable operations
    Status Put(SequenceNumber seq, const Slice& key, const Slice& value);
    Status Delete(SequenceNumber seq, const Slice& key);
    Status Get(const Slice& key, SequenceNumber snapshot_seq, std::string* value);

    // Creates an iterator over this CF's active memtable and immutable queue
    std::unique_ptr<Iterator> NewIterator(SequenceNumber snapshot_seq = kMaxSequenceNumber);

    // Memtable rotation & flush management
    std::shared_ptr<MemTableCore> GetActiveMemTable();
    void RotateMemTable();
    std::vector<std::shared_ptr<MemTableCore>> GetImmutableQueue();
    void ClearImmutableQueue();

    SequenceNumber GetLastFlushedSeq() const { return last_flushed_seq_.load(std::memory_order_acquire); }
    void SetLastFlushedSeq(SequenceNumber s) { last_flushed_seq_.store(s, std::memory_order_release); }

    SequenceNumber GetEarliestMemSeq() const { return earliest_mem_seq_.load(std::memory_order_acquire); }
    void SetEarliestMemSeq(SequenceNumber s) { earliest_mem_seq_.store(s, std::memory_order_release); }

    size_t TotalMemoryUsage() const;

    // Physical SSTable files associated with this column family
    const std::vector<FileMetaData>& GetLevelFiles(int level) const;
    void AddLevelFile(int level, const FileMetaData& f);

    ColumnFamilySet* GetSet() const { return set_; }

private:
    uint32_t id_{0};
    std::string name_{};
    ColumnFamilyOptions options_{};
    ColumnFamilySet* set_{nullptr};

    std::atomic<ColumnFamilyStatus> status_{ColumnFamilyStatus::kActive};
    std::atomic<uint32_t> refs_{1};

    mutable std::mutex mutex_{};
    std::shared_ptr<MemTableCore> mem_{};
    std::vector<std::shared_ptr<MemTableCore>> imm_queue_{};

    std::atomic<SequenceNumber> last_flushed_seq_{0};
    std::atomic<SequenceNumber> earliest_mem_seq_{0};

    std::vector<FileMetaData> files_[Version::kNumLevels];
};

// ColumnFamilyHandle: User-facing opaque handle to a specific column family
class ColumnFamilyHandle {
public:
    explicit ColumnFamilyHandle(std::shared_ptr<ColumnFamilyData> cfd)
        : cfd_(std::move(cfd)) {}
    ~ColumnFamilyHandle() = default;

    uint32_t GetID() const { return cfd_ ? cfd_->GetID() : 0; }
    const std::string& GetName() const {
        static const std::string kEmpty = "";
        return cfd_ ? cfd_->GetName() : kEmpty;
    }

    ColumnFamilyData* cfd() const { return cfd_.get(); }
    std::shared_ptr<ColumnFamilyData> SharedCFD() const { return cfd_; }

private:
    std::shared_ptr<ColumnFamilyData> cfd_{nullptr};
};

// ColumnFamilySet: Thread-safe registry mapping names and IDs to ColumnFamilyData (Feature 97)
class ColumnFamilySet {
public:
    ColumnFamilySet();
    ~ColumnFamilySet() = default;

    ColumnFamilySet(const ColumnFamilySet&) = delete;
    ColumnFamilySet& operator=(const ColumnFamilySet&) = delete;

    // Dynamic Creation and Drop
    std::unique_ptr<ColumnFamilyHandle> CreateColumnFamily(
        const std::string& name,
        const ColumnFamilyOptions& options = {}
    );

    Status DropColumnFamily(ColumnFamilyHandle* handle);

    // Lookups (O(1))
    std::unique_ptr<ColumnFamilyHandle> GetDefault();
    std::unique_ptr<ColumnFamilyHandle> GetColumnFamily(const std::string& name);
    std::unique_ptr<ColumnFamilyHandle> GetColumnFamily(uint32_t id);

    std::vector<std::unique_ptr<ColumnFamilyHandle>> ListColumnFamilies();
    std::vector<std::shared_ptr<ColumnFamilyData>> GetAllCFDs();

    size_t Count() const;

private:
    mutable std::mutex mutex_{};
    std::atomic<uint32_t> next_cf_id_{1}; // 0 reserved for "default"
    std::shared_ptr<ColumnFamilyData> default_cfd_{nullptr};

    std::unordered_map<std::string, std::shared_ptr<ColumnFamilyData>> name_to_cfd_{};
    std::unordered_map<uint32_t, std::shared_ptr<ColumnFamilyData>> id_to_cfd_{};
};

} // namespace lsm
