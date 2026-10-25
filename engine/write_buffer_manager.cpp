#include "write_buffer_manager.hpp"

namespace lsm {

WriteBufferManager::WriteBufferManager(size_t buffer_size_limit)
    : buffer_size_limit_(buffer_size_limit),
      flush_threshold_(static_cast<size_t>(buffer_size_limit * 0.90)),
      memory_allocated_(0) {}

void WriteBufferManager::SetLimit(size_t buffer_size_limit) {
    buffer_size_limit_.store(buffer_size_limit, std::memory_order_relaxed);
    flush_threshold_.store(static_cast<size_t>(buffer_size_limit * 0.90), std::memory_order_relaxed);
}

bool WriteBufferManager::ReserveMemory(size_t bytes) {
    size_t prev = memory_allocated_.fetch_add(bytes, std::memory_order_relaxed);
    size_t new_usage = prev + bytes;
    size_t threshold = flush_threshold_.load(std::memory_order_relaxed);

    return (new_usage >= threshold);
}

void WriteBufferManager::ReleaseMemory(size_t bytes) {
    size_t current = memory_allocated_.load(std::memory_order_relaxed);
    while (true) {
        size_t new_val = (current >= bytes) ? (current - bytes) : 0;
        if (memory_allocated_.compare_exchange_weak(current, new_val, std::memory_order_relaxed)) {
            break;
        }
    }
}

bool WriteBufferManager::IsBudgetExceeded() const {
    return memory_allocated_.load(std::memory_order_relaxed) >= buffer_size_limit_.load(std::memory_order_relaxed);
}

bool WriteBufferManager::ShouldFlush() const {
    return memory_allocated_.load(std::memory_order_relaxed) >= flush_threshold_.load(std::memory_order_relaxed);
}

void WriteBufferManager::Reset() {
    memory_allocated_.store(0, std::memory_order_relaxed);
}

} // namespace lsm
