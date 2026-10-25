#include "sequence_generator.hpp"

namespace lsm {

SequenceGenerator::SequenceGenerator(SequenceNumber initial_seq)
    : current_seq_(initial_seq) {}

SequenceNumber SequenceGenerator::Allocate(uint64_t count) {
    if (count == 0) {
        return current_seq_.load(std::memory_order_acquire);
    }
    // fetch_add returns previous value; the new range is [prev + 1, prev + count]
    uint64_t prev = current_seq_.fetch_add(count, std::memory_order_acq_rel);
    return prev + 1;
}

SequenceNumber SequenceGenerator::BatchAllocate(uint64_t count, SequenceNumber* out_start) {
    if (count == 0) {
        SequenceNumber cur = current_seq_.load(std::memory_order_acquire);
        if (out_start) *out_start = cur;
        return cur;
    }
    uint64_t prev = current_seq_.fetch_add(count, std::memory_order_acq_rel);
    uint64_t start_seq = prev + 1;
    uint64_t end_seq = prev + count;
    if (out_start) {
        *out_start = start_seq;
    }
    return end_seq;
}

void SequenceGenerator::SetCurrent(SequenceNumber seq) {
    uint64_t cur = current_seq_.load(std::memory_order_acquire);
    while (seq > cur) {
        if (current_seq_.compare_exchange_weak(cur, seq, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
}

SequenceNumber SequenceGenerator::Last() const {
    return current_seq_.load(std::memory_order_acquire);
}

void SequenceGenerator::Reset(SequenceNumber seq) {
    current_seq_.store(seq, std::memory_order_release);
}

} // namespace lsm
