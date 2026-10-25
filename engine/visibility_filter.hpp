#pragma once

#include "types.hpp"
#include "internal_key.hpp"

namespace lsm {

// VisibilityFilter: Inlined zero-overhead predicate evaluating record version visibility
// with respect to snapshot sequence numbers (Feature 82).
class VisibilityFilter {
public:
    // Basic sequence comparison: returns true if entry sequence is <= snapshot sequence.
    static constexpr bool IsVisible(SequenceNumber entry_seq, SequenceNumber snapshot_seq) noexcept {
        return entry_seq <= snapshot_seq;
    }

    // Evaluates visibility of a ParsedInternalKey
    static inline bool IsVisible(const ParsedInternalKey& ikey, SequenceNumber snapshot_seq) noexcept {
        return ikey.sequence <= snapshot_seq;
    }

    // Evaluates visibility of an InternalKey
    static inline bool IsVisible(const InternalKey& ikey, SequenceNumber snapshot_seq) noexcept {
        return ikey.sequence() <= snapshot_seq;
    }

    // Evaluates visibility of a raw internal key slice
    static inline bool IsVisible(const Slice& ikey_slice, SequenceNumber snapshot_seq) noexcept {
        uint64_t tag = ExtractTagFromInternalKey(ikey_slice);
        return ExtractSequenceNumber(tag) <= snapshot_seq;
    }

    // Returns true if entry sequence was committed strictly after snapshot creation (future write)
    static constexpr bool IsInvisibleFutureWrite(SequenceNumber entry_seq, SequenceNumber snapshot_seq) noexcept {
        return entry_seq > snapshot_seq;
    }
};

} // namespace lsm
