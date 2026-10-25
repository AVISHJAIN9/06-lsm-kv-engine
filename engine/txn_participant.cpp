#include "txn_participant.hpp"

namespace lsm {

TxnParticipant::TxnParticipant(
    PartitionId partition_id,
    std::shared_ptr<MemTableCore> memtable,
    std::shared_ptr<WoundWaitEngine> lock_engine
) : partition_id_(partition_id),
    memtable_(std::move(memtable)),
    lock_engine_(std::move(lock_engine)) {}

Status TxnParticipant::OnPrepare(
    TxnId txn_id,
    const std::vector<Mutation>& mutations,
    HLCTimestamp start_ts
) {
    if (simulated_fail_.load(std::memory_order_relaxed)) {
        return Status::IOError("Simulated participant prepare failure on partition " + std::to_string(partition_id_));
    }

    if (lock_engine_) {
        lock_engine_->RegisterTxn(txn_id, start_ts);
        for (const auto& m : mutations) {
            Status s = lock_engine_->AcquireLock(txn_id, m.key);
            if (!s.ok()) {
                lock_engine_->ReleaseAllLocks(txn_id);
                std::lock_guard<std::mutex> lock(mutex_);
                PreparedTxnContext ctx;
                ctx.txn_id = txn_id;
                ctx.start_ts = start_ts;
                ctx.state = TxnState::kAborted;
                prepared_txns_[txn_id] = ctx;
                return s;
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    PreparedTxnContext ctx;
    ctx.txn_id = txn_id;
    ctx.start_ts = start_ts;
    ctx.mutations = mutations;
    ctx.state = TxnState::kPrepared;
    prepared_txns_[txn_id] = ctx;

    return Status::OK();
}

Status TxnParticipant::OnCommit(
    TxnId txn_id,
    HLCTimestamp commit_ts,
    SequenceNumber seq
) {
    std::vector<Mutation> mutations_to_apply;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = prepared_txns_.find(txn_id);
        if (it == prepared_txns_.end()) {
            return Status::NotFound("Prepared transaction not found for commit: " + std::to_string(txn_id));
        }

        if (it->second.state != TxnState::kPrepared) {
            return Status::InvalidArgument("Transaction is not in PREPARED state");
        }

        it->second.state = TxnState::kCommitted;
        it->second.commit_ts = commit_ts;
        mutations_to_apply = it->second.mutations;
    }

    // Apply mutations to local MemTable
    if (memtable_) {
        SequenceNumber s = seq > 0 ? seq : (commit_ts.physical_time_ms * 1000 + commit_ts.logical_counter);
        if (s == 0) s = 1;
        for (const auto& m : mutations_to_apply) {
            if (m.type == ValueType::kTypeValue) {
                memtable_->Add(s, ValueType::kTypeValue, Slice(m.key), Slice(m.value));
            } else if (m.type == ValueType::kTypeDeletion) {
                memtable_->Add(s, ValueType::kTypeDeletion, Slice(m.key), Slice());
            }
        }
    }

    // Release locks
    if (lock_engine_) {
        lock_engine_->ReleaseAllLocks(txn_id);
        lock_engine_->UnregisterTxn(txn_id);
    }

    return Status::OK();
}

Status TxnParticipant::OnAbort(TxnId txn_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = prepared_txns_.find(txn_id);
        if (it != prepared_txns_.end()) {
            it->second.state = TxnState::kAborted;
        } else {
            PreparedTxnContext ctx;
            ctx.txn_id = txn_id;
            ctx.state = TxnState::kAborted;
            prepared_txns_[txn_id] = ctx;
        }
    }

    // Release locks
    if (lock_engine_) {
        lock_engine_->ReleaseAllLocks(txn_id);
        lock_engine_->UnregisterTxn(txn_id);
    }

    return Status::OK();
}

TxnState TxnParticipant::GetTxnState(TxnId txn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = prepared_txns_.find(txn_id);
    if (it == prepared_txns_.end()) {
        return TxnState::kIdle;
    }
    return it->second.state;
}

std::vector<TxnId> TxnParticipant::RecoverInDoubt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TxnId> in_doubt;
    for (const auto& kv : prepared_txns_) {
        if (kv.second.state == TxnState::kPrepared) {
            in_doubt.push_back(kv.first);
        }
    }
    return in_doubt;
}

Status TxnParticipant::ResolveInDoubt(TxnId txn_id, TxnState decision, HLCTimestamp commit_ts) {
    if (decision == TxnState::kCommitted) {
        return OnCommit(txn_id, commit_ts);
    } else if (decision == TxnState::kAborted) {
        return OnAbort(txn_id);
    }
    return Status::InvalidArgument("Invalid resolution decision for in-doubt transaction");
}

Status TxnParticipant::Get(const Slice& key, SequenceNumber snapshot_seq, std::string* value) const {
    if (!memtable_) {
        return Status::InvalidArgument("Null MemTable in TxnParticipant");
    }
    Status s;
    bool found = memtable_->Get(key, snapshot_seq, value, &s);
    if (!found) {
        return Status::NotFound("Key not found in participant MemTable");
    }
    return s;
}

} // namespace lsm
