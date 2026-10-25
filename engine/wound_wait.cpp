#include "wound_wait.hpp"

namespace lsm {

void WoundWaitEngine::RegisterTxn(TxnId txn_id, HLCTimestamp start_ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    TxnInfo info;
    info.txn_id = txn_id;
    info.start_ts = start_ts;
    info.wounded = false;
    txns_[txn_id] = info;
}

void WoundWaitEngine::UnregisterTxn(TxnId txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = txns_.find(txn_id);
    if (it != txns_.end()) {
        for (const auto& key : it->second.held_keys) {
            auto l_it = key_locks_.find(key);
            if (l_it != key_locks_.end() && l_it->second.holder_txn_id == txn_id) {
                l_it->second.is_locked = false;
                l_it->second.holder_txn_id = 0;
            }
        }
        txns_.erase(it);
        cv_.notify_all();
    }
}

Status WoundWaitEngine::AcquireLock(TxnId txn_id, const std::string& key, uint64_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it_req = txns_.find(txn_id);
    if (it_req == txns_.end()) {
        return Status::InvalidArgument("Transaction ID not registered: " + std::to_string(txn_id));
    }

    if (it_req->second.wounded) {
        return Status::Aborted("Transaction was wounded by an older transaction");
    }

    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        if (it_req->second.wounded) {
            return Status::Aborted("Transaction was wounded by an older transaction");
        }

        auto& key_entry = key_locks_[key];
        if (!key_entry.is_locked || key_entry.holder_txn_id == txn_id) {
            // Free or already held by self -> grant lock
            key_entry.is_locked = true;
            key_entry.holder_txn_id = txn_id;
            it_req->second.held_keys.insert(key);
            return Status::OK();
        }

        TxnId holder_id = key_entry.holder_txn_id;
        auto it_holder = txns_.find(holder_id);

        if (it_holder == txns_.end()) {
            // Holder unmapped -> take lock
            key_entry.is_locked = true;
            key_entry.holder_txn_id = txn_id;
            it_req->second.held_keys.insert(key);
            return Status::OK();
        }

        HLCTimestamp req_ts = it_req->second.start_ts;
        HLCTimestamp holder_ts = it_holder->second.start_ts;

        if (req_ts < holder_ts) {
            // Requestor is OLDER -> WOUND holder!
            it_holder->second.wounded = true;
            it_holder->second.held_keys.erase(key);

            key_entry.is_locked = true;
            key_entry.holder_txn_id = txn_id;
            it_req->second.held_keys.insert(key);

            cv_.notify_all();
            return Status::OK();
        } else {
            // Requestor is YOUNGER -> WAIT for holder to release
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time
            ).count();

            if (static_cast<uint64_t>(elapsed) >= timeout_ms) {
                return Status::Busy("Lock acquisition timed out waiting for key: " + key);
            }

            uint64_t remaining_ms = timeout_ms - elapsed;
            cv_.wait_for(lock, std::chrono::milliseconds(remaining_ms));
        }
    }
}

void WoundWaitEngine::ReleaseLock(TxnId txn_id, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it_txn = txns_.find(txn_id);
    if (it_txn != txns_.end()) {
        it_txn->second.held_keys.erase(key);
    }

    auto it_lock = key_locks_.find(key);
    if (it_lock != key_locks_.end() && it_lock->second.holder_txn_id == txn_id) {
        it_lock->second.is_locked = false;
        it_lock->second.holder_txn_id = 0;
        cv_.notify_all();
    }
}

void WoundWaitEngine::ReleaseAllLocks(TxnId txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it_txn = txns_.find(txn_id);
    if (it_txn != txns_.end()) {
        for (const auto& key : it_txn->second.held_keys) {
            auto it_lock = key_locks_.find(key);
            if (it_lock != key_locks_.end() && it_lock->second.holder_txn_id == txn_id) {
                it_lock->second.is_locked = false;
                it_lock->second.holder_txn_id = 0;
            }
        }
        it_txn->second.held_keys.clear();
        cv_.notify_all();
    }
}

bool WoundWaitEngine::IsWounded(TxnId txn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = txns_.find(txn_id);
    if (it == txns_.end()) return false;
    return it->second.wounded;
}

void WoundWaitEngine::MarkWounded(TxnId txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = txns_.find(txn_id);
    if (it != txns_.end()) {
        it->second.wounded = true;
        cv_.notify_all();
    }
}

size_t WoundWaitEngine::GetHeldLockCount(TxnId txn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = txns_.find(txn_id);
    if (it == txns_.end()) return 0;
    return it->second.held_keys.size();
}

} // namespace lsm
