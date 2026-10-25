#include "compaction_limiter.hpp"

namespace lsm {

CompactionRateLimiter::CompactionRateLimiter(
    uint64_t rate_bytes_per_sec,
    uint64_t burst_bytes
) : rate_bytes_per_sec_(rate_bytes_per_sec),
    burst_bytes_(burst_bytes),
    last_refill_time_(std::chrono::steady_clock::now()) {
    if (burst_bytes_ == 0 && rate_bytes_per_sec_ > 0) {
        // Default burst to 100ms worth of tokens
        burst_bytes_ = std::max<uint64_t>(4096, rate_bytes_per_sec_ / 10);
    }
    available_tokens_ = static_cast<int64_t>(burst_bytes_);
}

void CompactionRateLimiter::SetRateBytesPerSec(uint64_t rate_bytes_per_sec, uint64_t burst_bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    rate_bytes_per_sec_ = rate_bytes_per_sec;
    if (burst_bytes == 0 && rate_bytes_per_sec_ > 0) {
        burst_bytes_ = std::max<uint64_t>(4096, rate_bytes_per_sec_ / 10);
    } else {
        burst_bytes_ = burst_bytes;
    }
    available_tokens_ = std::min(available_tokens_, static_cast<int64_t>(burst_bytes_));
    last_refill_time_ = std::chrono::steady_clock::now();
}

uint64_t CompactionRateLimiter::GetRateBytesPerSec() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rate_bytes_per_sec_;
}

int64_t CompactionRateLimiter::GetAvailableTokens() const {
    std::lock_guard<std::mutex> lock(mu_);
    return available_tokens_;
}

void CompactionRateLimiter::Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    available_tokens_ = static_cast<int64_t>(burst_bytes_);
    last_refill_time_ = std::chrono::steady_clock::now();
}

void CompactionRateLimiter::RefillTokensLocked(const std::chrono::steady_clock::time_point& now) {
    if (rate_bytes_per_sec_ == 0) {
        return;
    }
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_refill_time_).count();
    if (elapsed_ns <= 0) {
        return;
    }

    double added_tokens = (static_cast<double>(elapsed_ns) / 1e9) * static_cast<double>(rate_bytes_per_sec_);
    available_tokens_ = std::min<int64_t>(
        static_cast<int64_t>(burst_bytes_),
        available_tokens_ + static_cast<int64_t>(added_tokens)
    );
    last_refill_time_ = now;
}

void CompactionRateLimiter::Request(size_t bytes) {
    if (bytes == 0) return;

    std::chrono::nanoseconds sleep_dur{0};
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (rate_bytes_per_sec_ == 0) {
            return; // Unlimited bandwidth
        }

        auto now = std::chrono::steady_clock::now();
        RefillTokensLocked(now);

        available_tokens_ -= static_cast<int64_t>(bytes);

        if (available_tokens_ < 0) {
            int64_t deficit = -available_tokens_;
            double seconds_needed = static_cast<double>(deficit) / static_cast<double>(rate_bytes_per_sec_);
            sleep_dur = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>(seconds_needed)
            );
        }
    }

    if (sleep_dur.count() > 0) {
        std::this_thread::sleep_for(sleep_dur);
    }
}

} // namespace lsm
