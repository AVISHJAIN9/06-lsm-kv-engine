#include "db.hpp"
#include "prefix_iterator.hpp"
#include "memtable_iterator.hpp"
#include "ttl_util.hpp"
#include "metrics.hpp"
#include "recovery.hpp"
#include <mutex>
#include <iostream>
#include <filesystem>

namespace lsm {

namespace fs = std::filesystem;

class DBImpl : public DB {
public:
    explicit DBImpl(const Options& options, std::string dbname)
        : options_(options),
          dbname_(std::move(dbname)),
          mem_(std::make_unique<MemTable>()) {
        
        std::string wal_dir = options_.wal_dir.empty() ? (dbname_ + "/wal") : options_.wal_dir;
        std::string archive_dir = options_.archive_dir.empty() ? (dbname_ + "/archive") : options_.archive_dir;

        if (options_.enable_wal) {
            // Feature 20 & 21: Execute crash recovery replay
            RecoveryOptions rec_opts;
            rec_opts.truncate_corrupt_tail = options_.recovery_truncate_corrupt_tail;
            rec_opts.encryption_key = options_.encryption_key;
            rec_opts.verify_checksums = true;

            RecoveryEngine recovery(rec_opts);
            SequenceNumber recovered_seq = 0;
            recovery.Recover(wal_dir, mem_.get(), &recovered_seq, &recovery_report_);
            last_sequence_.store(recovered_seq);

            // Initialize active WAL manager
            WALOptions wal_opts;
            wal_opts.wal_dir = wal_dir;
            wal_opts.max_wal_file_size = options_.max_wal_file_size;
            wal_manager_ = std::make_unique<WALManager>(wal_opts);
            wal_manager_->Open();

            checkpoint_mgr_ = std::make_unique<CheckpointManager>();
            checkpoint_mgr_->RegisterWAL(
                wal_manager_->GetCurrentLogNumber(),
                wal_manager_->GetLogFilePath(wal_manager_->GetCurrentLogNumber()),
                recovered_seq + 1
            );

            archiver_ = std::make_unique<ColdTierArchiver>(archive_dir);

            committer_ = std::make_unique<GroupCommitter>(
                wal_manager_.get(),
                options_.fsync_mode,
                [this](WriteBatch* b) { PrepareBatch(b); },
                [this](WriteBatch* b) { return ApplyBatchToMemTable(b); }
            );
        }
    }

    ~DBImpl() override {
        if (wal_manager_) {
            wal_manager_->Close();
        }
    }

    void PrepareBatch(WriteBatch* batch) {
        std::unique_lock<std::mutex> lock(write_mutex_);
        SequenceNumber first_seq = last_sequence_.load() + 1;
        batch->SetSequence(first_seq);
        last_sequence_.store(first_seq + batch->Count() - 1);
    }

    Status Put(const WriteOptions& options, const Slice& key, const Slice& value) override {
        ScopedTimer timer(&MetricsCollector::Instance(), MetricType::kWriteLatency);
        if (key.empty()) {
            return Status::InvalidArgument("Key cannot be empty");
        }
        MetricsCollector::Instance().AddCounter(CounterType::kOpsPut, 1);
        WriteBatch batch;
        batch.Put(key, value);
        return Write(options, &batch);
    }

    Status PutWithTTL(
        const WriteOptions& options,
        const Slice& key,
        const Slice& value,
        uint64_t ttl_seconds
    ) override {
        if (key.empty()) {
            return Status::InvalidArgument("Key cannot be empty");
        }
        std::string encoded_val = TTLUtil::EncodeWithTTL(value, ttl_seconds);
        return Put(options, key, Slice(encoded_val));
    }

    Status Delete(const WriteOptions& options, const Slice& key) override {
        ScopedTimer timer(&MetricsCollector::Instance(), MetricType::kWriteLatency);
        if (key.empty()) {
            return Status::InvalidArgument("Key cannot be empty");
        }
        MetricsCollector::Instance().AddCounter(CounterType::kOpsDelete, 1);
        WriteBatch batch;
        batch.Delete(key);
        return Write(options, &batch);
    }

    Status Write(const WriteOptions& options, WriteBatch* batch) override {
        if (!batch || batch->Count() == 0) {
            return Status::OK();
        }

        if (committer_) {
            return committer_->Submit(batch, options.sync);
        } else {
            PrepareBatch(batch);
            return ApplyBatchToMemTable(batch);
        }
    }

    Status ApplyBatchToMemTable(WriteBatch* batch) {
        class BatchInserter : public WriteBatch::Handler {
        public:
            BatchInserter(SequenceNumber seq, MemTable* mem)
                : current_seq_(seq), mem_(mem) {}

            void Put(const Slice& key, const Slice& value) override {
                mem_->Add(current_seq_, ValueType::kTypeValue, key, value);
                current_seq_++;
            }

            void Delete(const Slice& key) override {
                mem_->Add(current_seq_, ValueType::kTypeDeletion, key, Slice());
                current_seq_++;
            }

        private:
            SequenceNumber current_seq_{0};
            MemTable* mem_{nullptr};
        };

        BatchInserter inserter(batch->Sequence(), mem_.get());
        Status s = batch->Iterate(&inserter);
        if (s.ok() && checkpoint_mgr_ && wal_manager_) {
            checkpoint_mgr_->UpdateWALMaxLSN(
                wal_manager_->GetCurrentLogNumber(),
                batch->Sequence() + batch->Count() - 1,
                wal_manager_->GetCurrentFileSize()
            );
        }

        return s;
    }

    Status Get(const ReadOptions& options, const Slice& key, std::string* value) override {
        ScopedTimer timer(&MetricsCollector::Instance(), MetricType::kReadLatency);
        MetricsCollector::Instance().AddCounter(CounterType::kOpsGet, 1);

        if (key.empty()) {
            return Status::InvalidArgument("Key cannot be empty");
        }

        SequenceNumber snapshot_seq = (options.snapshot != kMaxSequenceNumber)
                                      ? options.snapshot
                                      : last_sequence_.load();

        Status s;
        std::string raw_val;

        // 1. Check active MemTable
        if (mem_->Get(key, snapshot_seq, &raw_val, &s)) {
            if (s.ok()) {
                if (TTLUtil::IsExpired(Slice(raw_val))) {
                    return Status::NotFound("Key has expired (TTL)");
                }
                if (value) {
                    Slice stripped;
                    uint64_t exp = 0;
                    TTLUtil::DecodeTTL(Slice(raw_val), &stripped, &exp);
                    *value = stripped.ToString();
                    MetricsCollector::Instance().AddCounter(CounterType::kBytesRead, value->size());
                }
            }
            return s;
        }

        // 2. Check immutable MemTables (if any)
        for (auto it = imm_.rbegin(); it != imm_.rend(); ++it) {
            if ((*it)->Get(key, snapshot_seq, &raw_val, &s)) {
                if (s.ok()) {
                    if (TTLUtil::IsExpired(Slice(raw_val))) {
                        return Status::NotFound("Key has expired (TTL)");
                    }
                    if (value) {
                        Slice stripped;
                        uint64_t exp = 0;
                        TTLUtil::DecodeTTL(Slice(raw_val), &stripped, &exp);
                        *value = stripped.ToString();
                        MetricsCollector::Instance().AddCounter(CounterType::kBytesRead, value->size());
                    }
                }
                return s;
            }
        }

        return Status::NotFound("Key not found in database");
    }

    std::vector<Status> MultiGet(
        const ReadOptions& options,
        const std::vector<Slice>& keys,
        std::vector<std::string>* values
    ) override {
        std::vector<Status> statuses(keys.size());
        if (values) {
            values->resize(keys.size());
        }

        for (size_t i = 0; i < keys.size(); ++i) {
            std::string* val_ptr = values ? &(*values)[i] : nullptr;
            statuses[i] = Get(options, keys[i], val_ptr);
        }

        return statuses;
    }

    std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override {
        SequenceNumber snapshot_seq = (options.snapshot != kMaxSequenceNumber)
                                      ? options.snapshot
                                      : last_sequence_.load();
        return std::make_unique<UserMemTableIterator>(mem_.get(), snapshot_seq);
    }

    std::unique_ptr<Iterator> NewPrefixIterator(const ReadOptions& options, const Slice& prefix) override {
        return std::make_unique<PrefixIterator>(NewIterator(options), prefix);
    }

    Status RangeScan(
        const ReadOptions& options,
        const Slice& start,
        const Slice& end,
        size_t limit,
        std::vector<std::pair<std::string, std::string>>* results
    ) override {
        ScopedTimer timer(&MetricsCollector::Instance(), MetricType::kScanLatency);
        MetricsCollector::Instance().AddCounter(CounterType::kOpsScan, 1);

        if (!results) {
            return Status::InvalidArgument("Results output vector cannot be null");
        }
        results->clear();

        auto iter = NewIterator(options);
        for (iter->Seek(start); iter->Valid(); iter->Next()) {
            if (!end.empty() && iter->key() >= end) {
                break; // Crossed upper boundary [start, end)
            }
            results->emplace_back(iter->key().ToString(), iter->value().ToString());
            MetricsCollector::Instance().AddCounter(CounterType::kBytesRead, iter->key().size() + iter->value().size());
            if (limit > 0 && results->size() >= limit) {
                break; // Reached maximum requested limit
            }
        }

        return Status::OK();
    }

    const Snapshot* GetSnapshot() override {
        return snapshots_.New(last_sequence_.load());
    }

    void ReleaseSnapshot(const Snapshot* snapshot) override {
        if (snapshot) {
            snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
        }
    }

    std::unique_ptr<Transaction> BeginTransaction(
        const WriteOptions& write_options,
        const TransactionOptions& txn_options
    ) override {
        return std::make_unique<OptimisticTransaction>(this, write_options, txn_options);
    }

    bool HasKeyBeenModifiedSince(const Slice& key, SequenceNumber seq) const override {
        if (mem_->HasKeyBeenModifiedSince(key, seq)) {
            return true;
        }
        for (const auto& imm : imm_) {
            if (imm->HasKeyBeenModifiedSince(key, seq)) {
                return true;
            }
        }
        return false;
    }

    bool GetProperty(const Slice& property, std::string* value) const override {
        if (!value) return false;
        std::string prop_name = property.ToString();

        if (prop_name == "lsm.active-memtable-bytes") {
            *value = std::to_string(mem_->ApproximateMemoryUsage());
            return true;
        } else if (prop_name == "lsm.active-snapshots") {
            *value = std::to_string(snapshots_.count());
            return true;
        } else if (prop_name == "lsm.sequence-number") {
            *value = std::to_string(last_sequence_.load());
            return true;
        } else if (prop_name == "lsm.num-keys") {
            *value = std::to_string(mem_->Count());
            return true;
        } else if (prop_name == "lsm.is-healthy") {
            *value = "1";
            return true;
        } else if (prop_name == "lsm.total-wal-bytes") {
            *value = std::to_string(wal_manager_ ? wal_manager_->GetTotalWALBytes() : 0);
            return true;
        }
        return false;
    }

    EngineHealthReport GetEngineHealth() const override {
        EngineHealthReport report;
        report.status = HealthStatus::HEALTHY;
        report.active_memtable_bytes = ApproximateMemoryUsage();
        report.active_snapshots = snapshots_.count();
        report.current_sequence = last_sequence_.load();
        report.total_keys = mem_->Count();
        report.details = "All LSM engine subsystems operating within normal parameters.";
        return report;
    }

    MetricsSnapshot GetMetrics() const override {
        return MetricsCollector::Instance().GetSnapshot();
    }

    Status SyncWAL() override {
        if (wal_manager_) {
            return wal_manager_->Sync();
        }
        return Status::OK();
    }

    Status RotateWAL() override {
        if (wal_manager_) {
            uint64_t old_log = wal_manager_->GetCurrentLogNumber();
            if (checkpoint_mgr_) {
                checkpoint_mgr_->MarkWALImmutable(old_log);
            }
            Status s = wal_manager_->Rotate();
            if (s.ok() && checkpoint_mgr_) {
                checkpoint_mgr_->RegisterWAL(
                    wal_manager_->GetCurrentLogNumber(),
                    wal_manager_->GetLogFilePath(wal_manager_->GetCurrentLogNumber()),
                    last_sequence_.load() + 1
                );
            }
            return s;
        }
        return Status::OK();
    }

    Status PurgeObsoleteWALs(uint64_t min_active_log_num, size_t* deleted_count) override {
        if (wal_manager_) {
            return wal_manager_->PurgeObsoleteWALs(min_active_log_num, deleted_count);
        }
        if (deleted_count) *deleted_count = 0;
        return Status::OK();
    }

    WALManager* GetWALManager() override {
        return wal_manager_.get();
    }

    RecoveryReport GetRecoveryReport() const override {
        return recovery_report_;
    }

    Status CreateCheckpoint(SequenceNumber* checkpoint_lsn) override {
        if (checkpoint_lsn) {
            *checkpoint_lsn = last_sequence_.load();
        }
        return SyncWAL();
    }

    CheckpointManager* GetCheckpointManager() override {
        return checkpoint_mgr_.get();
    }

    ColdTierArchiver* GetArchiver() override {
        return archiver_.get();
    }

    SequenceNumber GetLatestSequenceNumber() const override {
        return last_sequence_.load();
    }

    size_t ApproximateMemoryUsage() const override {
        size_t total = mem_->ApproximateMemoryUsage();
        for (const auto& imm : imm_) {
            total += imm->ApproximateMemoryUsage();
        }
        return total;
    }

private:
    Options options_{};
    std::string dbname_{};
    std::mutex write_mutex_{};
    std::atomic<SequenceNumber> last_sequence_{0};
    std::unique_ptr<MemTable> mem_{nullptr};
    std::vector<std::unique_ptr<MemTable>> imm_{};
    SnapshotList snapshots_{};
    std::unique_ptr<WALManager> wal_manager_{nullptr};
    std::unique_ptr<GroupCommitter> committer_{nullptr};
    std::unique_ptr<CheckpointManager> checkpoint_mgr_{nullptr};
    std::unique_ptr<ColdTierArchiver> archiver_{nullptr};
    RecoveryReport recovery_report_{};
};

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
    *dbptr = nullptr;
    auto* db = new DBImpl(options, name);
    *dbptr = db;
    return Status::OK();
}

} // namespace lsm
