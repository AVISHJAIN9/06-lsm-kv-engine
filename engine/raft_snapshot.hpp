#pragma once

#include <string>
#include <memory>
#include <mutex>
#include "status.hpp"
#include "raft_node.hpp"
#include "memtable_core.hpp"

namespace lsm {

struct SnapshotMeta {
    Index last_included_index{0};
    Term last_included_term{0};
    size_t data_size{0};
};

// RaftSnapshotManager: Raft log compaction engine linked to LSM SSTable checkpoints (Feature 124).
class RaftSnapshotManager {
public:
    explicit RaftSnapshotManager(std::shared_ptr<RaftNode> raft_node);
    ~RaftSnapshotManager() = default;

    RaftSnapshotManager(const RaftSnapshotManager&) = delete;
    RaftSnapshotManager& operator=(const RaftSnapshotManager&) = delete;

    // Creates an immutable checkpoint snapshot up to applied_index and truncates Raft log prefix
    Status CreateSnapshot(
        Index applied_index,
        const std::string& checkpoint_data
    );

    // Installs snapshot payload into the node, resetting state machine and restoring lagged followers
    Status InstallSnapshot(
        const InstallSnapshotArgs& args,
        std::shared_ptr<MemTableCore> target_memtable = nullptr
    );

    SnapshotMeta GetLatestSnapshotMeta() const;

private:
    std::shared_ptr<RaftNode> raft_node_{nullptr};
    mutable std::mutex mutex_{};
};

} // namespace lsm
