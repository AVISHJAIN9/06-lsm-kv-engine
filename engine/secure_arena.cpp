#include "secure_arena.hpp"
#include <algorithm>

namespace lsm {

void SecureArena::SecureZero(void* ptr, size_t size) {
    if (!ptr || size == 0) return;
    volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(ptr);
    while (size--) {
        *p++ = 0;
    }
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r"(ptr) : "memory");
#endif
}

SecureArena::SecureArena(size_t block_size)
    : block_size_(block_size) {}

SecureArena::~SecureArena() {
    WipeAndReset();
}

void SecureArena::WipeAndReset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& block : blocks_) {
        SecureZero(block.first, block.second);
        delete[] block.first;
    }
    blocks_.clear();
    alloc_ptr_ = nullptr;
    alloc_bytes_remaining_ = 0;
    memory_usage_.store(0, std::memory_order_relaxed);
}

char* SecureArena::AllocateNewBlock(size_t block_bytes) {
    char* block = new char[block_bytes];
    // Zero newly allocated memory
    SecureZero(block, block_bytes);
    blocks_.push_back({block, block_bytes});
    memory_usage_.fetch_add(block_bytes, std::memory_order_relaxed);
    return block;
}

char* SecureArena::Allocate(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes == 0) return nullptr;

    if (bytes <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        return result;
    }

    if (bytes > (block_size_ / 4)) {
        return AllocateNewBlock(bytes);
    }

    alloc_ptr_ = AllocateNewBlock(block_size_);
    alloc_bytes_remaining_ = block_size_ - bytes;
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    return result;
}

char* SecureArena::AllocateAligned(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
    static_assert((align & (align - 1)) == 0, "Alignment must be power of two");

    size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
    size_t slop = (current_mod == 0 ? 0 : align - current_mod);
    size_t needed = bytes + slop;

    if (needed <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_ + slop;
        alloc_ptr_ += needed;
        alloc_bytes_remaining_ -= needed;
        return result;
    }

    if (bytes > (block_size_ / 4)) {
        return AllocateNewBlock(bytes);
    }

    alloc_ptr_ = AllocateNewBlock(block_size_);
    alloc_bytes_remaining_ = block_size_ - bytes;
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    return result;
}

} // namespace lsm
