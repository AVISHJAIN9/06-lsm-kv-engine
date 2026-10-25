#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "hlc_snapshot.hpp"
#include "status.hpp"

namespace lsm {

using TxnId = uint64_t;

enum class LockMode {
    kShared,
    kExclusive
};

struct LockRequest {
    TxnId txn_id{0};
    HLCTimestamp start_ts;
    LockMode mode{LockMode::kExclusive};
    bool granted{false};
};

// WoundWaitEngine: Distributed deadlock prevention engine implementing Wound-Wait priority preemption
class WoundWaitEngine {
public:
    WoundWaitEngine() = default;
    ~WoundWaitEngine() = default;

    WoundWaitEngine(const WoundWaitEngine&) = delete;
    WoundWaitEngine& operator=(const WoundWaitEngine&) = delete;

    // Registers a transaction with its start timestamp
    void RegisterTxn(TxnId txn_id, HLCTimestamp start_ts);

    // Unregisters a transaction and cleans up tracking state
    void UnregisterTxn(TxnId txn_id);

    // Attempts to acquire an exclusive lock on 'key'.
    // If key is held:
    //   - Requestor is older (lower start_ts) -> Wounds holder (holder aborts).
    //   - Requestor is younger (higher start_ts) -> Requestor waits on CV.
    Status AcquireLock(TxnId txn_id, const std::string& key, uint64_t timeout_ms = 500);

    // Releases lock on 'key' held by txn_id
    void ReleaseLock(TxnId txn_id, const std::string& key);

    // Releases all locks held by txn_id
    void ReleaseAllLocks(TxnId txn_id);

    // Checks if transaction has been wounded by an older transaction
    bool IsWounded(TxnId txn_id) const;

    // Manually marks a transaction as wounded
    void MarkWounded(TxnId txn_id);

    // Returns the number of locks currently held by txn_id
    size_t GetHeldLockCount(TxnId txn_id) const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    struct TxnInfo {
        TxnId txn_id{0};
        HLCTimestamp start_ts;
        bool wounded{false};
        std::unordered_set<std::string> held_keys;
    };

    struct KeyLockEntry {
        TxnId holder_txn_id{0};
        bool is_locked{false};
    };

    std::unordered_map<TxnId, TxnInfo> txns_;
    std::unordered_map<std::string, KeyLockEntry> key_locks_;
};

} // namespace lsm
