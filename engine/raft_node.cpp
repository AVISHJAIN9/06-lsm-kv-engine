#include "raft_node.hpp"
#include <algorithm>
#include <iostream>

namespace lsm {

RaftNode::RaftNode(
    NodeId self_id,
    std::vector<NodeId> peers,
    std::shared_ptr<RaftNetworkTransport> transport
) : self_id_(self_id),
    peers_(std::move(peers)),
    transport_(std::move(transport)) {
    ResetElectionTimer();
}

void RaftNode::SetTransport(std::shared_ptr<RaftNetworkTransport> transport) {
    std::lock_guard<std::mutex> lock(mutex_);
    transport_ = std::move(transport);
}

RaftRole RaftNode::GetRole() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return role_;
}

Term RaftNode::GetCurrentTerm() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_term_;
}

NodeId RaftNode::GetLeaderId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_leader_;
}

Index RaftNode::GetCommitIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return commit_index_;
}

Index RaftNode::GetLastApplied() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_applied_;
}

void RaftNode::SetLastApplied(Index index) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_applied_ = index;
}

Index RaftNode::GetLastLogIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_last_index_ + log_.size();
}

Term RaftNode::GetLastLogTerm() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetLogTermAt(snapshot_last_index_ + log_.size());
}

Term RaftNode::GetLogTermAt(Index index) const {
    if (index == 0) return 0;
    if (index == snapshot_last_index_) return snapshot_last_term_;
    if (index > snapshot_last_index_ && index <= snapshot_last_index_ + log_.size()) {
        return log_[index - snapshot_last_index_ - 1].term;
    }
    return 0;
}

void RaftNode::ResetElectionTimer() {
    std::uniform_int_distribution<uint64_t> dist(150, 300);
    election_timeout_ms_ = dist(rng_);
    election_elapsed_ms_ = 0;
}

void RaftNode::BecomeFollower(Term new_term) {
    role_ = RaftRole::kFollower;
    current_term_ = new_term;
    voted_for_ = std::nullopt;
    ResetElectionTimer();
}

void RaftNode::BecomeCandidate() {
    role_ = RaftRole::kCandidate;
    current_term_++;
    voted_for_ = self_id_;
    current_leader_ = 0;
    ResetElectionTimer();
}

void RaftNode::BecomeLeader() {
    role_ = RaftRole::kLeader;
    current_leader_ = self_id_;
    Index last_idx = snapshot_last_index_ + log_.size();
    for (NodeId p : peers_) {
        next_index_[p] = last_idx + 1;
        match_index_[p] = 0;
    }
    heartbeat_elapsed_ms_ = heartbeat_interval_ms_; // Trigger immediate heartbeat
}

void RaftNode::TickElection(uint64_t elapsed_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (role_ == RaftRole::kLeader) return;

    election_elapsed_ms_ += elapsed_ms;
    if (election_elapsed_ms_ >= election_timeout_ms_) {
        lock.unlock();
        TriggerElection();
    }
}

void RaftNode::TickHeartbeat(uint64_t elapsed_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (role_ != RaftRole::kLeader) return;

    heartbeat_elapsed_ms_ += elapsed_ms;
    if (heartbeat_elapsed_ms_ >= heartbeat_interval_ms_) {
        heartbeat_elapsed_ms_ = 0;
        lock.unlock();
        BroadcastHeartbeats();
    }
}

void RaftNode::TriggerElection() {
    std::vector<NodeId> peers_copy;
    RequestVoteArgs args;
    Term election_term = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        BecomeCandidate();
        election_term = current_term_;
        peers_copy = peers_;

        args.term = election_term;
        args.candidate_id = self_id_;
        args.last_log_index = snapshot_last_index_ + log_.size();
        args.last_log_term = GetLogTermAt(args.last_log_index);
    }

    size_t total_voters = peers_copy.size() + 1;
    size_t quorum = (total_voters / 2) + 1;

    if (total_voters == 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (role_ == RaftRole::kCandidate && current_term_ == election_term) {
            BecomeLeader();
        }
        return;
    }

    auto votes_granted = std::make_shared<std::atomic<size_t>>(1); // Voted for self

    for (NodeId p : peers_copy) {
        if (!transport_) continue;
        transport_->SendRequestVote(
            p, args,
            [this, election_term, quorum, votes_granted](const RequestVoteReply& reply) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (reply.term > current_term_) {
                    BecomeFollower(reply.term);
                    return;
                }
                if (role_ == RaftRole::kCandidate && current_term_ == election_term && reply.vote_granted) {
                    size_t v = votes_granted->fetch_add(1) + 1;
                    if (v >= quorum && role_ == RaftRole::kCandidate) {
                        BecomeLeader();
                    }
                }
            }
        );
    }
}

RequestVoteReply RaftNode::HandleRequestVote(const RequestVoteArgs& args) {
    std::lock_guard<std::mutex> lock(mutex_);
    RequestVoteReply reply;
    reply.term = current_term_;
    reply.vote_granted = false;

    if (args.term > current_term_) {
        BecomeFollower(args.term);
    }

    if (args.term < current_term_) {
        return reply;
    }

    bool can_vote = (!voted_for_.has_value() || voted_for_.value() == args.candidate_id);
    Index my_last_index = snapshot_last_index_ + log_.size();
    Term my_last_term = GetLogTermAt(my_last_index);

    bool log_ok = (args.last_log_term > my_last_term) ||
                  (args.last_log_term == my_last_term && args.last_log_index >= my_last_index);

    if (can_vote && log_ok) {
        voted_for_ = args.candidate_id;
        reply.vote_granted = true;
        ResetElectionTimer();
    }

    reply.term = current_term_;
    return reply;
}

AppendEntriesReply RaftNode::HandleAppendEntries(const AppendEntriesArgs& args) {
    std::lock_guard<std::mutex> lock(mutex_);
    AppendEntriesReply reply;
    reply.term = current_term_;
    reply.success = false;

    if (args.term > current_term_) {
        BecomeFollower(args.term);
    }

    if (args.term < current_term_) {
        return reply;
    }

    // Acknowledge current leader and reset election timer
    role_ = RaftRole::kFollower;
    current_leader_ = args.leader_id;
    ResetElectionTimer();

    Index my_last_index = snapshot_last_index_ + log_.size();

    // Check if log contains an entry at prev_log_index with matching term
    if (args.prev_log_index > my_last_index) {
        reply.conflict_index = my_last_index + 1;
        reply.conflict_term = 0;
        return reply;
    }

    if (args.prev_log_index > 0 && GetLogTermAt(args.prev_log_index) != args.prev_log_term) {
        reply.conflict_term = GetLogTermAt(args.prev_log_index);
        // Find first index for this conflicting term
        Index first_idx = args.prev_log_index;
        while (first_idx > snapshot_last_index_ && GetLogTermAt(first_idx - 1) == reply.conflict_term) {
            first_idx--;
        }
        reply.conflict_index = first_idx;
        return reply;
    }

    // Append new entries not already present
    Index insert_idx = args.prev_log_index + 1;
    for (const auto& entry : args.entries) {
        if (insert_idx <= snapshot_last_index_ + log_.size()) {
            if (GetLogTermAt(insert_idx) != entry.term) {
                // Truncate mismatching tail
                size_t keep_count = insert_idx - snapshot_last_index_ - 1;
                log_.resize(keep_count);
                log_.push_back(entry);
            }
        } else {
            log_.push_back(entry);
        }
        insert_idx++;
    }

    // Update commit index
    if (args.leader_commit > commit_index_) {
        Index new_last = snapshot_last_index_ + log_.size();
        commit_index_ = std::min(args.leader_commit, new_last);
    }

    reply.success = true;
    reply.match_index = snapshot_last_index_ + log_.size();
    return reply;
}

InstallSnapshotReply RaftNode::HandleInstallSnapshot(const InstallSnapshotArgs& args) {
    std::lock_guard<std::mutex> lock(mutex_);
    InstallSnapshotReply reply;
    reply.term = current_term_;

    if (args.term > current_term_) {
        BecomeFollower(args.term);
    }
    if (args.term < current_term_) {
        return reply;
    }

    role_ = RaftRole::kFollower;
    current_leader_ = args.leader_id;
    ResetElectionTimer();

    if (args.last_included_index <= snapshot_last_index_) {
        return reply;
    }

    // Truncate or discard log
    if (args.last_included_index < snapshot_last_index_ + log_.size() &&
        GetLogTermAt(args.last_included_index) == args.last_included_term) {
        size_t discard = args.last_included_index - snapshot_last_index_;
        log_.erase(log_.begin(), log_.begin() + discard);
    } else {
        log_.clear();
    }

    snapshot_last_index_ = args.last_included_index;
    snapshot_last_term_ = args.last_included_term;
    snapshot_data_ = args.data;

    commit_index_ = std::max(commit_index_, snapshot_last_index_);
    last_applied_ = std::max(last_applied_, snapshot_last_index_);

    reply.term = current_term_;
    return reply;
}

Status RaftNode::Propose(
    const std::string& payload,
    RaftEntryType type,
    Index* out_index,
    Term* out_term
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (role_ != RaftRole::kLeader) {
        return Status::InvalidArgument("Not cluster leader (current leader: " +
                                      std::to_string(current_leader_) + ")");
    }

    Index new_idx = snapshot_last_index_ + log_.size() + 1;
    RaftLogEntry entry;
    entry.term = current_term_;
    entry.index = new_idx;
    entry.type = type;
    entry.payload = payload;
    log_.push_back(std::move(entry));

    if (out_index) *out_index = new_idx;
    if (out_term) *out_term = current_term_;

    // Single-node quorum check
    if (peers_.empty()) {
        commit_index_ = new_idx;
    }

    return Status::OK();
}

void RaftNode::BroadcastHeartbeats() {
    std::vector<NodeId> peers_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (role_ != RaftRole::kLeader) return;
        peers_copy = peers_;
    }

    for (NodeId p : peers_copy) {
        ReplicateToPeer(p);
    }
}

bool RaftNode::VerifyLeadershipQuorum() {
    if (!transport_) return false;

    std::vector<NodeId> peers_copy;
    Term cur_term = 0;
    NodeId leader = 0;
    Index commit_idx = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (role_ != RaftRole::kLeader) return false;
        peers_copy = peers_;
        cur_term = current_term_;
        leader = self_id_;
        commit_idx = commit_index_;
    }

    size_t acks = 1; // self
    size_t quorum = (peers_copy.size() + 1) / 2 + 1;
    if (acks >= quorum) return true;

    for (NodeId p : peers_copy) {
        AppendEntriesArgs args;
        args.term = cur_term;
        args.leader_id = leader;
        args.prev_log_index = 0;
        args.prev_log_term = 0;
        args.leader_commit = commit_idx;

        transport_->SendAppendEntries(p, args, [&](const AppendEntriesReply& reply) {
            if (reply.term == cur_term) {
                acks++;
            }
        });
    }

    return acks >= quorum;
}


void RaftNode::ReplicateToPeer(NodeId peer_id) {
    if (!transport_) return;

    AppendEntriesArgs args;
    InstallSnapshotArgs snap_args;
    bool send_snapshot = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (role_ != RaftRole::kLeader) return;

        Index next_idx = next_index_[peer_id];
        if (next_idx <= snapshot_last_index_) {
            send_snapshot = true;
            snap_args.term = current_term_;
            snap_args.leader_id = self_id_;
            snap_args.last_included_index = snapshot_last_index_;
            snap_args.last_included_term = snapshot_last_term_;
            snap_args.data = snapshot_data_;
        } else {
            args.term = current_term_;
            args.leader_id = self_id_;
            args.prev_log_index = next_idx - 1;
            args.prev_log_term = GetLogTermAt(args.prev_log_index);
            args.leader_commit = commit_index_;

            Index log_start = snapshot_last_index_;
            for (Index i = next_idx; i <= log_start + log_.size(); ++i) {
                args.entries.push_back(log_[i - log_start - 1]);
            }
        }
    }

    if (send_snapshot) {
        transport_->SendInstallSnapshot(
            peer_id, snap_args,
            [this, peer_id, snap_index = snap_args.last_included_index](const InstallSnapshotReply& reply) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (reply.term > current_term_) {
                    BecomeFollower(reply.term);
                    return;
                }
                if (role_ == RaftRole::kLeader) {
                    next_index_[peer_id] = snap_index + 1;
                    match_index_[peer_id] = snap_index;
                }
            }
        );
    } else {
        transport_->SendAppendEntries(
            peer_id, args,
            [this, peer_id, last_sent_idx = args.prev_log_index + args.entries.size()](const AppendEntriesReply& reply) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (reply.term > current_term_) {
                    BecomeFollower(reply.term);
                    return;
                }
                if (role_ != RaftRole::kLeader) return;

                if (reply.success) {
                    next_index_[peer_id] = last_sent_idx + 1;
                    match_index_[peer_id] = last_sent_idx;
                    AdvanceCommitIndex();
                } else {
                    if (reply.conflict_index > 0) {
                        next_index_[peer_id] = reply.conflict_index;
                    } else if (next_index_[peer_id] > 1) {
                        next_index_[peer_id]--;
                    }
                }
            }
        );
    }
}

void RaftNode::AdvanceCommitIndex() {
    if (role_ != RaftRole::kLeader) return;

    Index my_last = snapshot_last_index_ + log_.size();
    std::vector<Index> matches;
    matches.push_back(my_last);
    for (NodeId p : peers_) {
        matches.push_back(match_index_[p]);
    }
    std::sort(matches.begin(), matches.end(), std::greater<Index>());

    size_t quorum_idx = matches.size() / 2;
    Index median_match = matches[quorum_idx];

    if (median_match > commit_index_ && GetLogTermAt(median_match) == current_term_) {
        commit_index_ = median_match;
    }
}

Status RaftNode::CompactLog(
    Index last_included_index,
    Term last_included_term,
    const std::string& snapshot_data
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_included_index <= snapshot_last_index_) {
        return Status::OK();
    }

    if (last_included_index > snapshot_last_index_ + log_.size()) {
        return Status::InvalidArgument("Snapshot index exceeds log upper bound");
    }

    size_t discard_count = last_included_index - snapshot_last_index_;
    log_.erase(log_.begin(), log_.begin() + discard_count);

    snapshot_last_index_ = last_included_index;
    snapshot_last_term_ = last_included_term;
    snapshot_data_ = snapshot_data;

    return Status::OK();
}

std::string RaftNode::GetSnapshotData() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_data_;
}

Index RaftNode::GetSnapshotLastIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_last_index_;
}

Term RaftNode::GetSnapshotLastTerm() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_last_term_;
}

bool RaftNode::GetEntry(Index index, RaftLogEntry* out_entry) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index <= snapshot_last_index_ || index > snapshot_last_index_ + log_.size()) {
        return false;
    }
    if (out_entry) {
        *out_entry = log_[index - snapshot_last_index_ - 1];
    }
    return true;
}

std::vector<RaftLogEntry> RaftNode::GetEntriesFrom(Index start_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RaftLogEntry> result;
    if (start_index <= snapshot_last_index_) {
        start_index = snapshot_last_index_ + 1;
    }
    for (Index i = start_index; i <= snapshot_last_index_ + log_.size(); ++i) {
        result.push_back(log_[i - snapshot_last_index_ - 1]);
    }
    return result;
}

std::vector<NodeId> RaftNode::GetVoters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NodeId> voters = peers_;
    voters.push_back(self_id_);
    std::sort(voters.begin(), voters.end());
    return voters;
}

void RaftNode::SetVoters(const std::vector<NodeId>& voters) {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.clear();
    for (NodeId id : voters) {
        if (id != self_id_) {
            peers_.push_back(id);
            if (next_index_.find(id) == next_index_.end()) {
                next_index_[id] = snapshot_last_index_ + log_.size() + 1;
                match_index_[id] = 0;
            }
        }
    }
}

} // namespace lsm
