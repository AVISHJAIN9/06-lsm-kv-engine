#pragma once

#include <string>
#include <unordered_set>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "memtable_core.hpp"
#include "internal_key.hpp"
#include "visibility_filter.hpp"

namespace lsm {

// ConflictDetector: Multi-Version write-write and read-write conflict validator (Feature 89).
// Validates that no keys in a transaction's read-set or write-set have been concurrently
// modified after the transaction's snapshot sequence number.
class ConflictDetector {
public:
    ConflictDetector() = default;
    ~ConflictDetector() = default;

    // Checks read-set and write-set keys against active engine state.
    // Returns Status::Busy if any key was modified at sequence > snapshot_seq.
    static Status ValidateConflict(
        const std::unordered_set<std::string>& read_set,
        const std::unordered_set<std::string>& write_set,
        SequenceNumber snapshot_seq,
        MemTableCore* memtable
    );
};

} // namespace lsm
