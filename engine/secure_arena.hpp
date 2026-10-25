#pragma once

#include <cstdint>
#include <vector>
#include <atomic>
#include <cstddef>
#include <mutex>

namespace lsm {

// SecureArena: Memory scrubbing arena guaranteeing zero residual key/plaintext artifacts upon destruction (Feature 143)
class SecureArena {
public:
    explicit SecureArena(size_t block_size = 4096);
    ~SecureArena();

    SecureArena(const SecureArena&) = delete;
    SecureArena& operator=(const SecureArena&) = delete;

    // Allocates bytes from the secure arena
    char* Allocate(size_t bytes);

    // Allocates memory aligned to 8-byte boundary
    char* AllocateAligned(size_t bytes);

    // Explicitly wipes and resets all allocated blocks
    void WipeAndReset();

    // Returns total memory allocated by the arena
    size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }

    // Compiler-barrier volatile memory zeroing preventing dead-code elimination
    static void SecureZero(void* ptr, size_t size);

private:
    size_t block_size_{4096};
    char* alloc_ptr_{nullptr};
    size_t alloc_bytes_remaining_{0};
    std::vector<std::pair<char*, size_t>> blocks_;
    std::atomic<size_t> memory_usage_{0};
    mutable std::mutex mutex_;

    char* AllocateNewBlock(size_t block_bytes);
};

} // namespace lsm
