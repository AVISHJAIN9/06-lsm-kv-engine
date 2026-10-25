#pragma once

#include <set>
#include <string>
#include <memory>
#include <mutex>
#include "status.hpp"
#include "raft_node.hpp"

namespace lsm {

struct ClusterConfig {
    std::set<NodeId> voters{};
    std::set<NodeId> learners{};

    size_t QuorumSize() const {
        return (voters.size() / 2) + 1;
    }

    bool IsVoter(NodeId id) const {
        return voters.find(id) != voters.end();
    }

    bool IsLearner(NodeId id) const {
        return learners.find(id) != learners.end();
    }

    std::string Serialize() const;
    static ClusterConfig Deserialize(const std::string& data);
};

// MembershipManager: Dynamic cluster configuration changer and quorum membership validator (Feature 126).
class MembershipManager {
public:
    explicit MembershipManager(
        std::shared_ptr<RaftNode> raft_node,
        ClusterConfig initial_config = ClusterConfig{}
    );
    ~MembershipManager() = default;

    MembershipManager(const MembershipManager&) = delete;
    MembershipManager& operator=(const MembershipManager&) = delete;

    // Proposes adding a new voter or learner node to the cluster
    Status ProposeAddNode(NodeId id, bool is_voter = true);

    // Proposes removing a node from the cluster
    Status ProposeRemoveNode(NodeId id);

    // Applies a committed configuration transition (C_old -> C_new)
    void ApplyConfig(const ClusterConfig& new_config);

    ClusterConfig GetActiveConfig() const;
    size_t GetQuorumSize() const;

private:
    std::shared_ptr<RaftNode> raft_node_{nullptr};
    mutable std::mutex mutex_{};
    ClusterConfig config_{};
};

} // namespace lsm
