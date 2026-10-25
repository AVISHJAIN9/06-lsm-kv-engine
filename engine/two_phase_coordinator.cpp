#include "two_phase_coordinator.hpp"

namespace lsm {

TwoPhaseCoordinator::TwoPhaseCoordinator()
    : hlc_(std::make_shared<HybridLogicalClock>()) {}

TwoPhaseCoordinator::TwoPhaseCoordinator(std::shared_ptr<HybridLogicalClock> hlc)
    : hlc_(std::move(hlc)) {
    if (!hlc_) {
        hlc_ = std::make_shared<HybridLogicalClock>();
    }
}

DistributedTxn TwoPhaseCoordinator::BeginTransaction() {
    DistributedTxn txn;
    txn.txn_id = next_txn_id_.fetch_add(1, std::memory_order_relaxed);
    txn.start_ts = hlc_->Now();
    return txn;
}

Status TwoPhaseCoordinator::ExecuteTransaction(
    const DistributedTxn& txn,
    const std::map<PartitionId, std::shared_ptr<TxnParticipant>>& participants
) {
    if (txn.shard_mutations.empty()) {
        return Status::OK(); // Empty transaction is trivially committed
    }

    // Step 1: Compute HLC commit timestamp
    HLCTimestamp commit_ts = hlc_->Now();

    // Step 2: Phase 1 (Prepare) - Broadcast Prepare to all participant shards
    bool all_prepared = true;
    std::string failure_reason;

    for (const auto& kv : txn.shard_mutations) {
        PartitionId pid = kv.first;
        const auto& mutations = kv.second;

        auto it_p = participants.find(pid);
        if (it_p == participants.end() || !it_p->second) {
            all_prepared = false;
            failure_reason = "Participant for partition " + std::to_string(pid) + " not found";
            break;
        }

        Status s = it_p->second->OnPrepare(txn.txn_id, mutations, txn.start_ts);
        if (!s.ok()) {
            all_prepared = false;
            failure_reason = "Partition " + std::to_string(pid) + " prepare failed: " + s.ToString();
            break;
        }
    }

    // Step 3: Phase 2 (Commit or Abort)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (all_prepared) {
            txn_history_[txn.txn_id] = TxnState::kCommitted;
        } else {
            txn_history_[txn.txn_id] = TxnState::kAborted;
        }
    }

    if (all_prepared) {
        // Broadcast Commit to all participants
        for (const auto& kv : txn.shard_mutations) {
            PartitionId pid = kv.first;
            auto it_p = participants.find(pid);
            if (it_p != participants.end() && it_p->second) {
                it_p->second->OnCommit(txn.txn_id, commit_ts);
            }
        }
        return Status::OK();
    } else {
        // Broadcast Abort to all participants
        for (const auto& kv : txn.shard_mutations) {
            PartitionId pid = kv.first;
            auto it_p = participants.find(pid);
            if (it_p != participants.end() && it_p->second) {
                it_p->second->OnAbort(txn.txn_id);
            }
        }
        return Status::Aborted("2PC Transaction aborted: " + failure_reason);
    }
}

TxnState TwoPhaseCoordinator::GetTxnStatus(TxnId txn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = txn_history_.find(txn_id);
    if (it == txn_history_.end()) {
        return TxnState::kAborted; // Presumed abort for unknown / uncommitted transactions
    }
    return it->second;
}

void TwoPhaseCoordinator::RecordDecision(TxnId txn_id, TxnState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    txn_history_[txn_id] = state;
}

} // namespace lsm
