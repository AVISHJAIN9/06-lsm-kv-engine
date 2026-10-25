#pragma once

#include <memory>
#include <mutex>
#include <atomic>
#include <functional>
#include <vector>
#include "status.hpp"
#include "raft_node.hpp"
#include "memtable_core.hpp"
#include "sequence_generator.hpp"

namespace lsm {

using ApplyCallback = std::function<void(Index index, Term term, const std::string& payload)>;

// RaftApplier: Monotonic commit coordinator feeding the LSM MemTable state machine (Feature 123).
class RaftApplier {
public:
    explicit RaftApplier(std::shared_ptr<RaftNode> raft_node);
    ~RaftApplier() = default;

    RaftApplier(const RaftApplier&) = delete;
    RaftApplier& operator=(const RaftApplier&) = delete;

    // Scans newly committed entries (last_applied < commit_index) and applies them to the MemTable.
    // Returns the number of applied entries.
    size_t ApplyCommitted(
        std::shared_ptr<MemTableCore> target_memtable,
        SequenceGenerator* seq_gen = nullptr,
        ApplyCallback custom_callback = nullptr
    );

    Index GetLastApplied() const;

private:
    std::shared_ptr<RaftNode> raft_node_{nullptr};
    mutable std::mutex mutex_{};
    std::atomic<Index> last_applied_{0};
};

} // namespace lsm
