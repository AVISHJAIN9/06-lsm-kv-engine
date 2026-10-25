#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <atomic>
#include "status.hpp"

namespace lsm {

struct CFTrafficStats {
    uint32_t cf_id{0};
    double weight{1.0};
    uint64_t total_bytes_requested{0};
    uint64_t total_bytes_granted{0};
    uint64_t throttled_requests{0};
    uint64_t total_throttle_wait_us{0};
};

struct GlobalTrafficStats {
    size_t rate_limit_bytes_per_sec{0};
    uint64_t total_bytes_granted{0};
    uint64_t total_throttled_requests{0};
    size_t active_tenants{0};
};

// MultiCFRateLimiter: Fair-share token-bucket multi-tenant I/O bandwidth controller (Feature 104).
// Prevents noisy column families from starving lower-tier tenants (max 70% share when competing).
class MultiCFRateLimiter {
public:
    static constexpr double kMaxSingleTenantShare = 0.70;

    explicit MultiCFRateLimiter(
        size_t rate_limit_bytes_per_sec = 50 * 1024 * 1024, // 50MB/s default
        size_t burst_bytes = 10 * 1024 * 1024
    );
    ~MultiCFRateLimiter() = default;

    MultiCFRateLimiter(const MultiCFRateLimiter&) = delete;
    MultiCFRateLimiter& operator=(const MultiCFRateLimiter&) = delete;

    // Configures rate limit and weights
    void SetGlobalRateLimit(size_t bytes_per_sec);
    void SetCFWeight(uint32_t cf_id, double weight);

    // Requests bandwidth tokens for a compaction or flush job.
    // If tokens are available within fair-share bounds, grants immediately.
    // Otherwise, calculates required pacing delay and returns it in microseconds.
    uint64_t Request(uint32_t cf_id, size_t bytes, bool wait_if_throttled = false);

    CFTrafficStats GetCFTraffic(uint32_t cf_id) const;
    GlobalTrafficStats GetGlobalTraffic() const;

    void Reset();

private:
    struct CFState {
        double weight{1.0};
        double available_tokens{0.0};
        uint64_t total_requested{0};
        uint64_t total_granted{0};
        uint64_t throttled_count{0};
        uint64_t total_throttle_us{0};
    };

    void RefillTokensLocked(std::chrono::steady_clock::time_point now);

    mutable std::mutex mutex_{};
    size_t rate_limit_bytes_per_sec_{50 * 1024 * 1024};
    size_t burst_bytes_{10 * 1024 * 1024};
    double global_tokens_{0.0};
    std::chrono::steady_clock::time_point last_refill_time_{};

    std::unordered_map<uint32_t, CFState> cf_states_{};
    std::atomic<uint64_t> global_bytes_granted_{0};
    std::atomic<uint64_t> global_throttled_requests_{0};
};

} // namespace lsm
