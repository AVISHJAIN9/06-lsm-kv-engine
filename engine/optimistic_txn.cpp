#include "optimistic_txn.hpp"

namespace lsm {

OptimisticTxnContext::OptimisticTxnContext(
    SnapshotManager* snap_mgr,
    SequenceGenerator* seq_gen,
    MemTableCore* memtable
) : snap_mgr_(snap_mgr), seq_gen_(seq_gen), memtable_(memtable) {
    if (seq_gen_) {
        snapshot_seq_ = seq_gen_->Last();
    }
    if (snap_mgr_) {
        snapshot_handle_ = snap_mgr_->CreateSnapshot(snapshot_seq_);
    }
}

OptimisticTxnContext::~OptimisticTxnContext() {
    if (!committed_ && !rolled_back_) {
        Rollback();
    }
}

Status OptimisticTxnContext::Get(const Slice& key, std::string* value) {
    if (committed_ || rolled_back_) {
        return Status::InvalidArgument("Transaction already finalized");
    }

    std::string k_str = key.ToString();
    auto it = write_map_.find(k_str);
    if (it != write_map_.end()) {
        // Read-Your-Own-Writes from private local write buffer
        if (it->second.first == kTypeValue) {
            if (value) *value = it->second.second;
            return Status::OK();
        } else {
            return Status::NotFound("Key was deleted in local transaction buffer");
        }
    }

    // Fall back to engine lookup at snapshot sequence
    if (!memtable_) {
        return Status::NotFound("Key not found");
    }

    Status s;
    bool found = memtable_->Get(key, snapshot_seq_, value, &s);
    if (found && s.ok()) {
        return Status::OK();
    }
    return Status::NotFound("Key not found at snapshot");
}

Status OptimisticTxnContext::GetForUpdate(const Slice& key, std::string* value) {
    read_set_.insert(key.ToString());
    return Get(key, value);
}

Status OptimisticTxnContext::Put(const Slice& key, const Slice& value) {
    if (committed_ || rolled_back_) {
        return Status::InvalidArgument("Transaction already finalized");
    }

    write_map_[key.ToString()] = {kTypeValue, value.ToString()};
    write_batch_.Put(key, value);
    return Status::OK();
}

Status OptimisticTxnContext::Delete(const Slice& key) {
    if (committed_ || rolled_back_) {
        return Status::InvalidArgument("Transaction already finalized");
    }

    write_map_[key.ToString()] = {kTypeDeletion, ""};
    write_batch_.Delete(key);
    return Status::OK();
}

Status OptimisticTxnContext::Commit() {
    if (committed_ || rolled_back_) {
        return Status::InvalidArgument("Transaction already finalized");
    }

    if (write_map_.empty()) {
        // Read-only transaction commit
        committed_ = true;
        if (snap_mgr_ && snapshot_handle_) {
            snap_mgr_->ReleaseSnapshot(snapshot_handle_);
            snapshot_handle_ = nullptr;
        }
        return Status::OK();
    }

    // Phase 1: Conflict detection
    std::unordered_set<std::string> write_set_keys;
    for (const auto& kv : write_map_) {
        write_set_keys.insert(kv.first);
    }

    Status s = ConflictDetector::ValidateConflict(read_set_, write_set_keys, snapshot_seq_, memtable_);
    if (!s.ok()) {
        return s;
    }

    // Phase 2: Atomic Batch Commit
    s = AtomicBatchCommitter::Commit(&write_batch_, seq_gen_, memtable_);
    if (!s.ok()) {
        return s;
    }

    committed_ = true;
    if (snap_mgr_ && snapshot_handle_) {
        snap_mgr_->ReleaseSnapshot(snapshot_handle_);
        snapshot_handle_ = nullptr;
    }
    return Status::OK();
}

Status OptimisticTxnContext::Rollback() {
    if (committed_) {
        return Status::InvalidArgument("Cannot rollback already committed transaction");
    }

    write_map_.clear();
    write_batch_.Clear();
    read_set_.clear();
    rolled_back_ = true;

    if (snap_mgr_ && snapshot_handle_) {
        snap_mgr_->ReleaseSnapshot(snapshot_handle_);
        snapshot_handle_ = nullptr;
    }
    return Status::OK();
}

} // namespace lsm
