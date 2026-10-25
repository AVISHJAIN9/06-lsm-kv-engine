#pragma once

#include <string>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "write_batch.hpp"
#include "snapshot.hpp"

namespace lsm {

class DB;

struct TransactionOptions {
    bool set_snapshot{true};
};

class Transaction {
public:
    virtual ~Transaction() = default;

    virtual Status Get(const ReadOptions& options, const Slice& key, std::string* value) = 0;
    virtual Status GetForUpdate(const ReadOptions& options, const Slice& key, std::string* value) = 0;
    virtual Status Put(const Slice& key, const Slice& value) = 0;
    virtual Status Delete(const Slice& key) = 0;
    virtual Status Commit() = 0;
    virtual Status Rollback() = 0;
};

class OptimisticTransaction : public Transaction {
public:
    OptimisticTransaction(DB* db, const WriteOptions& write_options, const TransactionOptions& txn_options);
    ~OptimisticTransaction() override;

    Status Get(const ReadOptions& options, const Slice& key, std::string* value) override;
    Status GetForUpdate(const ReadOptions& options, const Slice& key, std::string* value) override;
    Status Put(const Slice& key, const Slice& value) override;
    Status Delete(const Slice& key) override;
    Status Commit() override;
    Status Rollback() override;

    SequenceNumber GetSnapshotSequence() const { return snapshot_seq_; }

private:
    DB* db_{nullptr};
    WriteOptions write_options_{};
    TransactionOptions txn_options_{};
    SequenceNumber snapshot_seq_{0};
    const Snapshot* snapshot_handle_{nullptr};

    WriteBatch batch_{};
    std::unordered_set<std::string> read_set_{};
    std::unordered_map<std::string, std::pair<ValueType, std::string>> write_set_{};
    bool committed_{false};
    bool rolled_back_{false};
};

} // namespace lsm
