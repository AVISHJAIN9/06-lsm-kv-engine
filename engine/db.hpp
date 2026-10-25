#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <utility>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "iterator.hpp"
#include "write_batch.hpp"
#include "memtable.hpp"
#include "snapshot.hpp"
#include "transaction.hpp"
#include "admin.hpp"
#include "metrics.hpp"
#include "wal_format.hpp"
#include "wal_writer.hpp"
#include "wal_reader.hpp"
#include "wal_manager.hpp"
#include "wal_committer.hpp"
#include "group_commit.hpp"
#include "checkpoint.hpp"
#include "wal_archival.hpp"
#include "wal_crypto.hpp"
#include "recovery.hpp"

namespace lsm {

struct Options {
    size_t write_buffer_size{4 * 1024 * 1024};
    bool create_if_missing{true};
    bool error_if_exists{false};
    bool enable_wal{true};
    std::string wal_dir{""};
    uint64_t max_wal_file_size{64 * 1024 * 1024};
    FsyncMode fsync_mode{FsyncMode::kFsyncNever};
    bool recovery_truncate_corrupt_tail{true};
    std::string encryption_key{""};
    std::string archive_dir{""};
};

class DB {
public:
    static Status Open(const Options& options, const std::string& name, DB** dbptr);

    DB() = default;
    virtual ~DB() = default;

    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

    // Feature 1, 2, 3: Basic Point Operations
    virtual Status Put(const WriteOptions& options, const Slice& key, const Slice& value) = 0;
    virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;
    virtual Status Write(const WriteOptions& options, WriteBatch* batch) = 0;
    virtual Status Get(const ReadOptions& options, const Slice& key, std::string* value) = 0;

    // Feature 4: Scatter-Gather MultiGet
    virtual std::vector<Status> MultiGet(
        const ReadOptions& options,
        const std::vector<Slice>& keys,
        std::vector<std::string>* values
    ) = 0;

    // Feature 6: Prefix Scan
    virtual std::unique_ptr<Iterator> NewPrefixIterator(const ReadOptions& options, const Slice& prefix) = 0;

    // Feature 7: Bounded Range Scan [start, end)
    virtual Status RangeScan(
        const ReadOptions& options,
        const Slice& start,
        const Slice& end,
        size_t limit,
        std::vector<std::pair<std::string, std::string>>* results
    ) = 0;

    // Feature 8: Bidirectional Iterator
    virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;

    // Feature 9: Point-in-time Consistent Snapshots
    virtual const Snapshot* GetSnapshot() = 0;
    virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

    // Feature 10: Optimistic Transactions
    virtual std::unique_ptr<Transaction> BeginTransaction(
        const WriteOptions& write_options = WriteOptions(),
        const TransactionOptions& txn_options = TransactionOptions()
    ) = 0;
    virtual bool HasKeyBeenModifiedSince(const Slice& key, SequenceNumber seq) const = 0;

    // Feature 11: TTL & Key Expiration
    virtual Status PutWithTTL(
        const WriteOptions& options,
        const Slice& key,
        const Slice& value,
        uint64_t ttl_seconds
    ) = 0;

    // Feature 12: Admin & Engine Health Diagnostic API
    virtual bool GetProperty(const Slice& property, std::string* value) const = 0;
    virtual EngineHealthReport GetEngineHealth() const = 0;

    // Feature 13: Client Metrics & Telemetry API
    virtual MetricsSnapshot GetMetrics() const = 0;

    // Feature 16 & 18: WAL Durability & Segment Management
    virtual Status SyncWAL() = 0;
    virtual Status RotateWAL() = 0;
    virtual Status PurgeObsoleteWALs(uint64_t min_active_log_num, size_t* deleted_count = nullptr) = 0;
    virtual WALManager* GetWALManager() = 0;

    // Feature 20, 21: Recovery Report
    virtual RecoveryReport GetRecoveryReport() const = 0;

    // Feature 22: Checkpoint Policy
    virtual Status CreateCheckpoint(SequenceNumber* checkpoint_lsn) = 0;
    virtual CheckpointManager* GetCheckpointManager() = 0;

    // Feature 23: Cold-Tier Archival
    virtual ColdTierArchiver* GetArchiver() = 0;

    virtual SequenceNumber GetLatestSequenceNumber() const = 0;
    virtual size_t ApproximateMemoryUsage() const = 0;
};

} // namespace lsm
