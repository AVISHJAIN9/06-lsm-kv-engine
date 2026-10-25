#include "raft_applier.hpp"

namespace lsm {

RaftApplier::RaftApplier(std::shared_ptr<RaftNode> raft_node)
    : raft_node_(std::move(raft_node)) {
    if (raft_node_) {
        last_applied_.store(raft_node_->GetLastApplied(), std::memory_order_relaxed);
    }
}

Index RaftApplier::GetLastApplied() const {
    return last_applied_.load(std::memory_order_acquire);
}

size_t RaftApplier::ApplyCommitted(
    std::shared_ptr<MemTableCore> target_memtable,
    SequenceGenerator* seq_gen,
    ApplyCallback custom_callback
) {
    if (!raft_node_) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    Index commit_idx = raft_node_->GetCommitIndex();
    Index curr = last_applied_.load(std::memory_order_relaxed);
    size_t applied_count = 0;

    while (curr < commit_idx) {
        curr++;
        RaftLogEntry entry;
        if (!raft_node_->GetEntry(curr, &entry)) {
            break;
        }

        if (entry.type == RaftEntryType::kData) {
            if (target_memtable) {
                SequenceNumber seq = seq_gen ? seq_gen->Allocate(1) : curr;
                const std::string& p = entry.payload;

                if (p.rfind("DEL:", 0) == 0) {
                    std::string key = p.substr(4);
                    target_memtable->Add(seq, ValueType::kTypeDeletion, Slice(key), Slice());
                } else if (p.rfind("PUT:", 0) == 0) {
                    size_t sep = p.rfind(':');
                    if (sep != std::string::npos && sep > 4) {
                        std::string key = p.substr(4, sep - 4);
                        std::string val = p.substr(sep + 1);
                        target_memtable->Add(seq, ValueType::kTypeValue, Slice(key), Slice(val));
                    }
                } else {
                    size_t eq = p.find('=');
                    if (eq != std::string::npos) {
                        std::string key = p.substr(0, eq);
                        std::string val = p.substr(eq + 1);
                        target_memtable->Add(seq, ValueType::kTypeValue, Slice(key), Slice(val));
                    } else {
                        target_memtable->Add(seq, ValueType::kTypeValue, Slice(p), Slice(p));
                    }
                }
            }
        }

        if (custom_callback) {
            custom_callback(entry.index, entry.term, entry.payload);
        }

        last_applied_.store(curr, std::memory_order_release);
        raft_node_->SetLastApplied(curr);
        applied_count++;
    }

    return applied_count;
}

} // namespace lsm
