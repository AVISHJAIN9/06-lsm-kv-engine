#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <new>

namespace lsm {

class Arena {
public:
    Arena();
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Return a pointer to a newly allocated memory block of "bytes" size.
    char* Allocate(size_t bytes);

    // Allocate memory with alignment (e.g. 8 bytes)
    char* AllocateAligned(size_t bytes);

    // Returns an estimate of the total memory allocated by the arena.
    size_t MemoryUsage() const {
        return memory_usage_.load(std::memory_order_relaxed);
    }

private:
    static constexpr size_t kBlockSize = 65536; // 64KB blocks

    char* alloc_ptr_{nullptr};
    size_t alloc_bytes_remaining_{0};

    // Array of allocated memory blocks.
    std::vector<char*> blocks_{};

    // Total memory allocated by the arena.
    std::atomic<size_t> memory_usage_{0};

    char* AllocateFallback(size_t bytes);
    char* AllocateNewBlock(size_t block_bytes);
};

inline Arena::Arena() {
    alloc_ptr_ = nullptr;
    alloc_bytes_remaining_ = 0;
    memory_usage_.store(0, std::memory_order_relaxed);
}

inline Arena::~Arena() {
    for (char* block : blocks_) {
        delete[] block;
    }
}

inline char* Arena::Allocate(size_t bytes) {
    assert(bytes > 0);
    if (bytes <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        return result;
    }
    return AllocateFallback(bytes);
}

inline char* Arena::AllocateFallback(size_t bytes) {
    if (bytes > kBlockSize / 4) {
        // Object is more than a quarter of our block size. Allocate it separately
        // to avoid wasting too much space in allocation blocks.
        char* result = AllocateNewBlock(bytes);
        return result;
    }

    // We waste the remaining bytes in the current block.
    alloc_ptr_ = AllocateNewBlock(kBlockSize);
    alloc_bytes_remaining_ = kBlockSize;

    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
}

inline char* Arena::AllocateAligned(size_t bytes) {
    constexpr size_t align = alignof(std::max_align_t);
    static_assert((align & (align - 1)) == 0, "Pointer size must be power of 2");

    size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
    size_t slop = (current_mod == 0 ? 0 : align - current_mod);
    size_t needed = bytes + slop;

    char* result;
    if (needed <= alloc_bytes_remaining_) {
        result = alloc_ptr_ + slop;
        alloc_ptr_ += needed;
        alloc_bytes_remaining_ -= needed;
    } else {
        // AllocateFallback always returns aligned memory for new blocks
        result = AllocateFallback(bytes);
    }
    assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
    return result;
}

inline char* Arena::AllocateNewBlock(size_t block_bytes) {
    char* block = new char[block_bytes];
    blocks_.push_back(block);
    memory_usage_.fetch_add(block_bytes + sizeof(char*), std::memory_order_relaxed);
    return block;
}

} // namespace lsm
