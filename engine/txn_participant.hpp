#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include "types.hpp"
#include "slice.hpp"
#include "status.hpp"
#include "hlc_snapshot.hpp"
#include "wound_wait.hpp"
#include "memtable_core.hpp"

namespace lsm {

using PartitionId = uint64_t;

enum class TxnState : uint8_t {
    kIdle = 0,
    kPrepared = 1,
    kCommitted = 2,
    kAborted = 3
};

struct Mutation {
    ValueType type{ValueType::kTypeValue};
    std::string key;
    std::string value;

    Mutation() = default;
    Mutation(ValueType t, std::string k, std::string v = "")
        : type(t), key(std::move(k)), value(std::move(v)) {}
};

struct PreparedTxnContext {
    TxnId txn_id{0};
    HLCTimestamp start_ts;
    HLCTimestamp commit_ts;
    std::vector<Mutation> mutations;
    TxnState state{TxnState::kIdle};
};

// TxnParticipant: Manages local shard 2PC state machine, lock acquisition, and prepared intent persistence
class TxnParticipant {
public:
    TxnParticipant(
        PartitionId partition_id,
        std::shared_ptr<MemTableCore> memtable,
        std::shared_ptr<WoundWaitEngine> lock_engine
    );
    ~TxnParticipant() = default;

    TxnParticipant(const TxnParticipant&) = delete;
    TxnParticipant& operator=(const TxnParticipant&) = delete;

    PartitionId GetPartitionId() const { return partition_id_; }

    // Phase 1: Prepare - acquires key locks and records prepared intent
    Status OnPrepare(
        TxnId txn_id,
        const std::vector<Mutation>& mutations,
        HLCTimestamp start_ts
    );

    // Phase 2: Commit - applies mutations to local MemTable and releases locks
    Status OnCommit(
        TxnId txn_id,
        HLCTimestamp commit_ts,
        SequenceNumber seq = 0
    );

    // Phase 2: Abort - discards prepared mutations and releases locks
    Status OnAbort(TxnId txn_id);

    // State query
    TxnState GetTxnState(TxnId txn_id) const;

    // Crash recovery: returns list of in-doubt prepared transactions requiring coordinator resolution
    std::vector<TxnId> RecoverInDoubt() const;

    // Resolves an in-doubt transaction based on coordinator decision
    Status ResolveInDoubt(TxnId txn_id, TxnState decision, HLCTimestamp commit_ts = {});

    // Injects a simulated fault on prepare (e.g. for negative testing)
    void SetSimulatedPrepareFailure(bool fail) { simulated_fail_.store(fail, std::memory_order_relaxed); }

    // Read with lock-intent check
    Status Get(const Slice& key, SequenceNumber snapshot_seq, std::string* value) const;

private:
    PartitionId partition_id_{0};
    std::shared_ptr<MemTableCore> memtable_;
    std::shared_ptr<WoundWaitEngine> lock_engine_;
    std::atomic<bool> simulated_fail_{false};

    mutable std::mutex mutex_;
    std::unordered_map<TxnId, PreparedTxnContext> prepared_txns_;
};

} // namespace lsm
