#include "multi_cf_limiter.hpp"
#include <thread>
#include <algorithm>

namespace lsm {

MultiCFRateLimiter::MultiCFRateLimiter(
    size_t rate_limit_bytes_per_sec,
    size_t burst_bytes
) : rate_limit_bytes_per_sec_(rate_limit_bytes_per_sec),
    burst_bytes_(burst_bytes),
    global_tokens_(static_cast<double>(burst_bytes)),
    last_refill_time_(std::chrono::steady_clock::now()) {}

void MultiCFRateLimiter::SetGlobalRateLimit(size_t bytes_per_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_limit_bytes_per_sec_ = bytes_per_sec;
}

void MultiCFRateLimiter::SetCFWeight(uint32_t cf_id, double weight) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (weight <= 0.0) weight = 0.1;
    auto& st = cf_states_[cf_id];
    st.weight = weight;
    if (st.available_tokens == 0.0) {
        double max_share = std::min(weight / 2.0, kMaxSingleTenantShare);
        st.available_tokens = static_cast<double>(burst_bytes_) * max_share;
    }
}

void MultiCFRateLimiter::RefillTokensLocked(std::chrono::steady_clock::time_point now) {
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_refill_time_).count();
    if (elapsed_us <= 0) return;

    last_refill_time_ = now;
    double new_tokens = (static_cast<double>(rate_limit_bytes_per_sec_) * elapsed_us) / 1000000.0;
    global_tokens_ = std::min(static_cast<double>(burst_bytes_), global_tokens_ + new_tokens);

    // Calculate sum of active weights
    double total_weight = 0.0;
    for (const auto& [_, st] : cf_states_) {
        total_weight += st.weight;
    }
    if (total_weight <= 0.0) total_weight = 1.0;

    // Distribute new tokens according to weights with fair share
    for (auto& [_, st] : cf_states_) {
        double share = st.weight / total_weight;
        double cf_max_share = std::min(share, kMaxSingleTenantShare);
        double cf_new_tokens = new_tokens * cf_max_share;
        double max_cf_burst = static_cast<double>(burst_bytes_) * cf_max_share;
        st.available_tokens = std::min(max_cf_burst, st.available_tokens + cf_new_tokens);
    }
}

uint64_t MultiCFRateLimiter::Request(uint32_t cf_id, size_t bytes, bool wait_if_throttled) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    RefillTokensLocked(now);

    auto& st = cf_states_[cf_id];
    st.total_requested += bytes;

    double req_d = static_cast<double>(bytes);
    if (global_tokens_ >= req_d && st.available_tokens >= req_d) {
        global_tokens_ -= req_d;
        st.available_tokens -= req_d;
        st.total_granted += bytes;
        global_bytes_granted_.fetch_add(bytes, std::memory_order_relaxed);
        return 0;
    }

    // Tenant throttled
    st.throttled_count++;
    global_throttled_requests_.fetch_add(1, std::memory_order_relaxed);

    double missing_tokens = std::max(req_d - st.available_tokens, req_d - global_tokens_);
    double effective_rate = (static_cast<double>(rate_limit_bytes_per_sec_) * st.weight);
    if (effective_rate <= 0.0) effective_rate = 1.0;

    uint64_t wait_us = static_cast<uint64_t>((missing_tokens * 1000000.0) / effective_rate);
    if (wait_us == 0) wait_us = 10;
    st.total_throttle_us += wait_us;

    if (wait_if_throttled) {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
        lock.lock();
        now = std::chrono::steady_clock::now();
        RefillTokensLocked(now);
        st.total_granted += bytes;
        global_bytes_granted_.fetch_add(bytes, std::memory_order_relaxed);
    }

    return wait_us;
}

CFTrafficStats MultiCFRateLimiter::GetCFTraffic(uint32_t cf_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cf_states_.find(cf_id);
    if (it == cf_states_.end()) {
        CFTrafficStats empty;
        empty.cf_id = cf_id;
        return empty;
    }
    const auto& st = it->second;
    CFTrafficStats res;
    res.cf_id = cf_id;
    res.weight = st.weight;
    res.total_bytes_requested = st.total_requested;
    res.total_bytes_granted = st.total_granted;
    res.throttled_requests = st.throttled_count;
    res.total_throttle_wait_us = st.total_throttle_us;
    return res;
}

GlobalTrafficStats MultiCFRateLimiter::GetGlobalTraffic() const {
    std::lock_guard<std::mutex> lock(mutex_);
    GlobalTrafficStats res;
    res.rate_limit_bytes_per_sec = rate_limit_bytes_per_sec_;
    res.total_bytes_granted = global_bytes_granted_.load(std::memory_order_relaxed);
    res.total_throttled_requests = global_throttled_requests_.load(std::memory_order_relaxed);
    res.active_tenants = cf_states_.size();
    return res;
}

void MultiCFRateLimiter::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    global_tokens_ = static_cast<double>(burst_bytes_);
    last_refill_time_ = std::chrono::steady_clock::now();
    for (auto& [_, st] : cf_states_) {
        st.available_tokens = static_cast<double>(burst_bytes_) * std::min(st.weight / 2.0, kMaxSingleTenantShare);
        st.total_requested = 0;
        st.total_granted = 0;
        st.throttled_count = 0;
        st.total_throttle_us = 0;
    }
    global_bytes_granted_.store(0, std::memory_order_relaxed);
    global_throttled_requests_.store(0, std::memory_order_relaxed);
}

} // namespace lsm
