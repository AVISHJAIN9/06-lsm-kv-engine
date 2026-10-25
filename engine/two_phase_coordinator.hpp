#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include "status.hpp"
#include "hlc_snapshot.hpp"
#include "txn_participant.hpp"

namespace lsm {

struct DistributedTxn {
    TxnId txn_id{0};
    HLCTimestamp start_ts;
    std::map<PartitionId, std::vector<Mutation>> shard_mutations;

    void Put(PartitionId pid, std::string key, std::string value) {
        shard_mutations[pid].emplace_back(ValueType::kTypeValue, std::move(key), std::move(value));
    }

    void Delete(PartitionId pid, std::string key) {
        shard_mutations[pid].emplace_back(ValueType::kTypeDeletion, std::move(key), "");
    }
};

// TwoPhaseCoordinator: Manages distributed 2PC lifecycle across arbitrary shard partitions
class TwoPhaseCoordinator {
public:
    TwoPhaseCoordinator();
    explicit TwoPhaseCoordinator(std::shared_ptr<HybridLogicalClock> hlc);
    ~TwoPhaseCoordinator() = default;

    TwoPhaseCoordinator(const TwoPhaseCoordinator&) = delete;
    TwoPhaseCoordinator& operator=(const TwoPhaseCoordinator&) = delete;

    // Begins a new distributed transaction, assigning monotonic TxnId and HLC start timestamp
    DistributedTxn BeginTransaction();

    // Executes 2PC protocol across participant shards
    Status ExecuteTransaction(
        const DistributedTxn& txn,
        const std::map<PartitionId, std::shared_ptr<TxnParticipant>>& participants
    );

    // Queries global coordinator decision for in-doubt transaction recovery
    TxnState GetTxnStatus(TxnId txn_id) const;

    // Manually records a decision (e.g. for testing recovery paths)
    void RecordDecision(TxnId txn_id, TxnState state);

    std::shared_ptr<HybridLogicalClock> GetHLC() const { return hlc_; }

private:
    std::shared_ptr<HybridLogicalClock> hlc_;
    std::atomic<uint64_t> next_txn_id_{1};

    mutable std::mutex mutex_;
    std::unordered_map<TxnId, TxnState> txn_history_;
};

} // namespace lsm
