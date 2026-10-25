#pragma once

#include <cstdint>
#include <atomic>
#include <algorithm>
#include "types.hpp"

namespace lsm {

// SequenceGenerator: Thread-safe, lock-free central 64-bit sequence counter (LSN)
// managing monotonic sequence allocations for single writes and contiguous atomic batches.
class SequenceGenerator {
public:
    explicit SequenceGenerator(SequenceNumber initial_seq = 0);
    ~SequenceGenerator() = default;

    // Non-copyable and non-movable
    SequenceGenerator(const SequenceGenerator&) = delete;
    SequenceGenerator& operator=(const SequenceGenerator&) = delete;
    SequenceGenerator(SequenceGenerator&&) = delete;
    SequenceGenerator& operator=(SequenceGenerator&&) = delete;

    // Allocates 'count' contiguous sequence numbers atomically.
    // Returns the first allocated sequence number in the batch range: [seq, seq + count - 1].
    SequenceNumber Allocate(uint64_t count = 1);

    // Batch allocate helper: returns the last assigned sequence number and optionally
    // writes the starting sequence number to out_start.
    SequenceNumber BatchAllocate(uint64_t count, SequenceNumber* out_start = nullptr);

    // Updates global sequence counter during recovery if 'seq' exceeds current sequence.
    // Thread-safe CAS loop ensuring sequence never regresses.
    void SetCurrent(SequenceNumber seq);

    // Returns the most recently allocated sequence number.
    SequenceNumber Last() const;

    // Resets sequence counter to specific value (e.g. for testing or isolated recovery).
    void Reset(SequenceNumber seq = 0);

private:
    std::atomic<uint64_t> current_seq_{0};
};

} // namespace lsm
