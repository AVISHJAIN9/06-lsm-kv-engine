#include "transaction.hpp"
#include "db.hpp"

namespace lsm {

OptimisticTransaction::OptimisticTransaction(
    DB* db,
    const WriteOptions& write_options,
    const TransactionOptions& txn_options
) : db_(db), write_options_(write_options), txn_options_(txn_options) {
    if (txn_options_.set_snapshot) {
        snapshot_handle_ = db_->GetSnapshot();
        snapshot_seq_ = snapshot_handle_->GetSequenceNumber();
    } else {
        snapshot_seq_ = db_->GetLatestSequenceNumber();
    }
}

OptimisticTransaction::~OptimisticTransaction() {
    if (snapshot_handle_ && db_) {
        db_->ReleaseSnapshot(snapshot_handle_);
        snapshot_handle_ = nullptr;
    }
}

Status OptimisticTransaction::Get(const ReadOptions& options, const Slice& key, std::string* value) {
    if (committed_) return Status::InvalidArgument("Transaction already committed");
    if (rolled_back_) return Status::InvalidArgument("Transaction already rolled back");

    std::string k_str = key.ToString();

    // 1. Check local uncommitted write-set
    auto it = write_set_.find(k_str);
    if (it != write_set_.end()) {
        if (it->second.first == ValueType::kTypeDeletion) {
            return Status::NotFound("Key deleted in current transaction");
        } else {
            if (value) *value = it->second.second;
            return Status::OK();
        }
    }

    // 2. Record key in read-set
    read_set_.insert(k_str);

    // 3. Perform read from DB at transaction snapshot sequence
    ReadOptions read_opts = options;
    read_opts.snapshot = snapshot_seq_;
    return db_->Get(read_opts, key, value);
}

Status OptimisticTransaction::GetForUpdate(const ReadOptions& options, const Slice& key, std::string* value) {
    return Get(options, key, value);
}

Status OptimisticTransaction::Put(const Slice& key, const Slice& value) {
    if (committed_) return Status::InvalidArgument("Transaction already committed");
    if (rolled_back_) return Status::InvalidArgument("Transaction already rolled back");

    std::string k_str = key.ToString();
    std::string v_str = value.ToString();
    write_set_[k_str] = {ValueType::kTypeValue, v_str};
    batch_.Put(key, value);
    return Status::OK();
}

Status OptimisticTransaction::Delete(const Slice& key) {
    if (committed_) return Status::InvalidArgument("Transaction already committed");
    if (rolled_back_) return Status::InvalidArgument("Transaction already rolled back");

    std::string k_str = key.ToString();
    write_set_[k_str] = {ValueType::kTypeDeletion, ""};
    batch_.Delete(key);
    return Status::OK();
}

Status OptimisticTransaction::Commit() {
    if (committed_) return Status::InvalidArgument("Transaction already committed");
    if (rolled_back_) return Status::InvalidArgument("Transaction already rolled back");

    if (batch_.Count() == 0) {
        committed_ = true;
        return Status::OK();
    }

    // Validate conflicts against read-set and write-set
    for (const auto& rk : read_set_) {
        if (db_->HasKeyBeenModifiedSince(Slice(rk), snapshot_seq_)) {
            return Status::Busy("Write-write conflict on read key: " + rk);
        }
    }

    for (const auto& [wk, _] : write_set_) {
        if (read_set_.find(wk) == read_set_.end()) {
            if (db_->HasKeyBeenModifiedSince(Slice(wk), snapshot_seq_)) {
                return Status::Busy("Write-write conflict on write key: " + wk);
            }
        }
    }

    Status s = db_->Write(write_options_, &batch_);
    if (s.ok()) {
        committed_ = true;
    }
    return s;
}

Status OptimisticTransaction::Rollback() {
    if (committed_) return Status::InvalidArgument("Cannot rollback committed transaction");
    if (rolled_back_) return Status::OK();

    batch_.Clear();
    read_set_.clear();
    write_set_.clear();
    rolled_back_ = true;
    return Status::OK();
}

} // namespace lsm
