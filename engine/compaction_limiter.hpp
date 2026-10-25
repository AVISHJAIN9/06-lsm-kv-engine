#pragma once

#include <cstdint>
#include <chrono>
#include <mutex>
#include <thread>
#include <algorithm>

namespace lsm {

// -----------------------------------------------------------------------------
// CompactionRateLimiter: High-precision Token-Bucket I/O Rate Limiter
// Prevents background compaction disk writes from saturating storage channels
// -----------------------------------------------------------------------------
class CompactionRateLimiter {
public:
    explicit CompactionRateLimiter(
        uint64_t rate_bytes_per_sec = 0, // 0 = unlimited
        uint64_t burst_bytes = 0         // 0 = defaults to 100ms burst
    );

    ~CompactionRateLimiter() = default;

    // Requests permission to write `bytes`. If tokens are insufficient,
    // the calling thread blocks until sufficient tokens are replenished.
    void Request(size_t bytes);

    void SetRateBytesPerSec(uint64_t rate_bytes_per_sec, uint64_t burst_bytes = 0);
    uint64_t GetRateBytesPerSec() const;
    int64_t GetAvailableTokens() const;

    void Reset();

private:
    void RefillTokensLocked(const std::chrono::steady_clock::time_point& now);

    mutable std::mutex mu_;
    uint64_t rate_bytes_per_sec_{0};
    uint64_t burst_bytes_{0};
    int64_t available_tokens_{0};
    std::chrono::steady_clock::time_point last_refill_time_;
};

} // namespace lsm
