#pragma once

#include <string>
#include <vector>
#include <utility>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "iterator.hpp"
#include "internal_key.hpp"
#include "visibility_filter.hpp"

namespace lsm {

// MVCCResolver: Deterministic multi-version read resolver extracting the exact visible
// record version across multi-tiered iterators respecting snapshot isolation (Feature 84).
class MVCCResolver {
public:
    // Resolves point lookup for target_user_key at snapshot_seq using an underlying Iterator.
    // Populates out_val, optional out_seq, and returns Status::OK() if visible value found,
    // or Status::NotFound() if deleted or nonexistent.
    static Status ResolvePointLookup(
        Iterator* iter,
        const Slice& target_user_key,
        SequenceNumber snapshot_seq,
        std::string* out_val,
        SequenceNumber* out_seq = nullptr,
        ValueType* out_type = nullptr
    );

    // Extracts complete chronological history of physical versions for target_user_key.
    // Returns list of (SequenceNumber, Value string) pairs in descending sequence order.
    static Status ResolveMultiVersionHistory(
        Iterator* iter,
        const Slice& target_user_key,
        std::vector<std::pair<SequenceNumber, std::string>>* out_history
    );

    // Evaluates whether target_user_key's latest visible version at snapshot_seq is a tombstone.
    static bool IsKeyDeletedAtSnapshot(
        Iterator* iter,
        const Slice& target_user_key,
        SequenceNumber snapshot_seq
    );
};

} // namespace lsm
