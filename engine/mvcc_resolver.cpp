#include "mvcc_resolver.hpp"

namespace lsm {

Status MVCCResolver::ResolvePointLookup(
    Iterator* iter,
    const Slice& target_user_key,
    SequenceNumber snapshot_seq,
    std::string* out_val,
    SequenceNumber* out_seq,
    ValueType* out_type
) {
    if (!iter) {
        return Status::Corruption("Null iterator provided to MVCCResolver");
    }

    // Construct seek key with snapshot_seq and highest value type to land at or before newest visible version
    InternalKey seek_ikey(target_user_key, snapshot_seq, kTypeValue);
    iter->Seek(seek_ikey.Encode());

    while (iter->Valid()) {
        Slice cur_ikey_slice = iter->key();
        ParsedInternalKey parsed;
        if (!ParseInternalKey(cur_ikey_slice, &parsed)) {
            return Status::Corruption("Corrupt internal key during MVCC resolution");
        }

        // Check if we advanced past the target user key
        if (parsed.user_key.compare(target_user_key) != 0) {
            break;
        }

        // Check visibility
        if (VisibilityFilter::IsVisible(parsed, snapshot_seq)) {
            if (out_seq) *out_seq = parsed.sequence;
            if (out_type) *out_type = parsed.type;

            if (parsed.type == kTypeValue) {
                if (out_val) {
                    out_val->assign(iter->value().data(), iter->value().size());
                }
                return Status::OK();
            } else if (parsed.type == kTypeDeletion) {
                return Status::NotFound("Key was deleted at snapshot");
            } else {
                return Status::Corruption("Unknown ValueType in internal key");
            }
        }

        iter->Next();
    }

    return Status::NotFound("Key not found at snapshot");
}

Status MVCCResolver::ResolveMultiVersionHistory(
    Iterator* iter,
    const Slice& target_user_key,
    std::vector<std::pair<SequenceNumber, std::string>>* out_history
) {
    if (!iter || !out_history) {
        return Status::Corruption("Invalid parameters to ResolveMultiVersionHistory");
    }
    out_history->clear();

    InternalKey seek_ikey(target_user_key, kMaxSequenceNumber, kTypeValue);
    iter->Seek(seek_ikey.Encode());

    while (iter->Valid()) {
        Slice cur_ikey_slice = iter->key();
        ParsedInternalKey parsed;
        if (!ParseInternalKey(cur_ikey_slice, &parsed)) {
            return Status::Corruption("Corrupt internal key during MVCC history extraction");
        }

        if (parsed.user_key.compare(target_user_key) != 0) {
            break;
        }

        std::string val_repr;
        if (parsed.type == kTypeValue) {
            val_repr.assign(iter->value().data(), iter->value().size());
        } else if (parsed.type == kTypeDeletion) {
            val_repr = "<TOMBSTONE_DELETED>";
        } else {
            val_repr = "<UNKNOWN_TYPE>";
        }

        out_history->emplace_back(parsed.sequence, std::move(val_repr));
        iter->Next();
    }

    return Status::OK();
}

bool MVCCResolver::IsKeyDeletedAtSnapshot(
    Iterator* iter,
    const Slice& target_user_key,
    SequenceNumber snapshot_seq
) {
    std::string val;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    Status s = ResolvePointLookup(iter, target_user_key, snapshot_seq, &val, &seq, &type);
    return (s.IsNotFound() && type == kTypeDeletion);
}

} // namespace lsm
