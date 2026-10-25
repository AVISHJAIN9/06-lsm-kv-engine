#include "raft_snapshot.hpp"

namespace lsm {

RaftSnapshotManager::RaftSnapshotManager(std::shared_ptr<RaftNode> raft_node)
    : raft_node_(std::move(raft_node)) {}

Status RaftSnapshotManager::CreateSnapshot(
    Index applied_index,
    const std::string& checkpoint_data
) {
    if (!raft_node_) {
        return Status::InvalidArgument("Null RaftNode in RaftSnapshotManager");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    Term term = 0;
    RaftLogEntry entry;
    if (raft_node_->GetEntry(applied_index, &entry)) {
        term = entry.term;
    } else {
        term = raft_node_->GetCurrentTerm();
    }

    return raft_node_->CompactLog(applied_index, term, checkpoint_data);
}

Status RaftSnapshotManager::InstallSnapshot(
    const InstallSnapshotArgs& args,
    std::shared_ptr<MemTableCore> target_memtable
) {
    if (!raft_node_) {
        return Status::InvalidArgument("Null RaftNode in RaftSnapshotManager");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    raft_node_->HandleInstallSnapshot(args);

    if (target_memtable && !args.data.empty()) {
        // Reset target memtable and populate with snapshot key-values
        // format: "key1=val1;key2=val2;"
        size_t start = 0;
        SequenceNumber snap_seq = args.last_included_index;
        while (start < args.data.size()) {
            size_t semi = args.data.find(';', start);
            if (semi == std::string::npos) semi = args.data.size();
            std::string item = args.data.substr(start, semi - start);
            size_t eq = item.find('=');
            if (eq != std::string::npos) {
                std::string k = item.substr(0, eq);
                std::string v = item.substr(eq + 1);
                target_memtable->Add(snap_seq, ValueType::kTypeValue, Slice(k), Slice(v));
            }
            start = semi + 1;
        }
    }

    return Status::OK();
}

SnapshotMeta RaftSnapshotManager::GetLatestSnapshotMeta() const {
    SnapshotMeta meta;
    if (raft_node_) {
        meta.last_included_index = raft_node_->GetSnapshotLastIndex();
        meta.last_included_term = raft_node_->GetSnapshotLastTerm();
        meta.data_size = raft_node_->GetSnapshotData().size();
    }
    return meta;
}

} // namespace lsm
