#include "raft_membership.hpp"
#include <sstream>
#include <vector>

namespace lsm {

std::string ClusterConfig::Serialize() const {
    std::ostringstream oss;
    oss << "VOTERS:";
    for (NodeId v : voters) {
        oss << v << ",";
    }
    oss << ";LEARNERS:";
    for (NodeId l : learners) {
        oss << l << ",";
    }
    return oss.str();
}

ClusterConfig ClusterConfig::Deserialize(const std::string& data) {
    ClusterConfig config;
    size_t voters_pos = data.find("VOTERS:");
    size_t learners_pos = data.find(";LEARNERS:");

    if (voters_pos != std::string::npos && learners_pos != std::string::npos) {
        std::string v_str = data.substr(7, learners_pos - 7);
        std::stringstream ss_v(v_str);
        std::string item;
        while (std::getline(ss_v, item, ',')) {
            if (!item.empty()) {
                config.voters.insert(std::stoull(item));
            }
        }

        std::string l_str = data.substr(learners_pos + 10);
        std::stringstream ss_l(l_str);
        while (std::getline(ss_l, item, ',')) {
            if (!item.empty()) {
                config.learners.insert(std::stoull(item));
            }
        }
    }
    return config;
}

MembershipManager::MembershipManager(
    std::shared_ptr<RaftNode> raft_node,
    ClusterConfig initial_config
) : raft_node_(std::move(raft_node)),
    config_(std::move(initial_config)) {
    if (config_.voters.empty() && raft_node_) {
        config_.voters.insert(raft_node_->GetId());
        for (NodeId v : raft_node_->GetVoters()) {
            config_.voters.insert(v);
        }
    }
}

Status MembershipManager::ProposeAddNode(NodeId id, bool is_voter) {
    if (!raft_node_) return Status::InvalidArgument("Null RaftNode");

    std::lock_guard<std::mutex> lock(mutex_);
    if (is_voter && config_.IsVoter(id)) {
        return Status::OK(); // Already voter
    }
    if (!is_voter && config_.IsLearner(id)) {
        return Status::OK(); // Already learner
    }

    ClusterConfig new_config = config_;
    if (is_voter) {
        new_config.learners.erase(id);
        new_config.voters.insert(id);
    } else {
        new_config.voters.erase(id);
        new_config.learners.insert(id);
    }

    std::string payload = "CONFIG_ADD:" + new_config.Serialize();
    Index out_idx = 0;
    Term out_term = 0;
    Status s = raft_node_->Propose(payload, RaftEntryType::kConfiguration, &out_idx, &out_term);
    if (!s.ok()) return s;

    // Immediately switch local configuration as per Raft specification
    ApplyConfig(new_config);
    return Status::OK();
}

Status MembershipManager::ProposeRemoveNode(NodeId id) {
    if (!raft_node_) return Status::InvalidArgument("Null RaftNode");

    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.IsVoter(id) && !config_.IsLearner(id)) {
        return Status::NotFound("Node not in cluster");
    }

    ClusterConfig new_config = config_;
    new_config.voters.erase(id);
    new_config.learners.erase(id);

    std::string payload = "CONFIG_REMOVE:" + new_config.Serialize();
    Index out_idx = 0;
    Term out_term = 0;
    Status s = raft_node_->Propose(payload, RaftEntryType::kConfiguration, &out_idx, &out_term);
    if (!s.ok()) return s;

    ApplyConfig(new_config);
    return Status::OK();
}

void MembershipManager::ApplyConfig(const ClusterConfig& new_config) {
    config_ = new_config;
    if (raft_node_) {
        std::vector<NodeId> voters_vec(config_.voters.begin(), config_.voters.end());
        raft_node_->SetVoters(voters_vec);
    }
}

ClusterConfig MembershipManager::GetActiveConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

size_t MembershipManager::GetQuorumSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.QuorumSize();
}

} // namespace lsm
