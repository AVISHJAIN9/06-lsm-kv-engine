#include "write_benchmark.hpp"
#include <chrono>
#include <thread>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <random>

namespace lsm {

std::string BenchmarkResult::ToString() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "Write-Path Benchmark Results:\n"
        << "  - Elapsed Time:       " << total_time_ms << " ms\n"
        << "  - Throughput:         " << ops_per_sec << " ops/sec\n"
        << "  - Bandwidth:          " << mb_per_sec << " MB/sec\n"
        << "  - P50 Write Latency:  " << p50_latency_ns << " ns\n"
        << "  - P90 Write Latency:  " << p90_latency_ns << " ns\n"
        << "  - P99 Write Latency:  " << p99_latency_ns << " ns\n"
        << "  - P99.9 Latency:      " << p999_latency_ns << " ns\n";
    return oss.str();
}

BenchmarkResult WriteBenchmarkRunner::Run(MemTableQueue* queue, const BenchmarkOptions& options) {
    BenchmarkResult res;
    if (!queue || options.num_keys == 0 || options.num_threads == 0) {
        return res;
    }

    const size_t keys_per_thread = options.num_keys / options.num_threads;
    std::vector<std::vector<uint64_t>> thread_latencies(options.num_threads);
    for (auto& lat : thread_latencies) {
        lat.reserve(keys_per_thread);
    }

    std::atomic<bool> start_flag{false};
    std::atomic<uint64_t> global_seq{1};

    auto worker = [&](size_t tid) {
        std::vector<uint64_t>& lats = thread_latencies[tid];
        std::mt19937_64 rng(1337 + tid);

        std::string val_payload(options.val_size, 'x');

        while (!start_flag.load(std::memory_order_acquire)) {}

        for (size_t i = 0; i < keys_per_thread; ++i) {
            uint64_t k_num = options.random_keys ? rng() : (tid * keys_per_thread + i);
            char key_buf[32];
            std::snprintf(key_buf, sizeof(key_buf), "k_%016llu", static_cast<unsigned long long>(k_num));
            Slice k(key_buf);
            Slice v(val_payload);

            uint64_t seq = global_seq.fetch_add(1, std::memory_order_relaxed);

            auto t0 = std::chrono::steady_clock::now();
            queue->Add(seq, ValueType::kTypeValue, k, v);
            auto t1 = std::chrono::steady_clock::now();

            uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            lats.push_back(lat_ns);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(options.num_threads);
    for (size_t t = 0; t < options.num_threads; ++t) {
        threads.emplace_back(worker, t);
    }

    auto start_time = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }
    auto end_time = std::chrono::steady_clock::now();

    res.total_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    std::vector<uint64_t> all_latencies;
    all_latencies.reserve(options.num_keys);
    for (const auto& lats : thread_latencies) {
        all_latencies.insert(all_latencies.end(), lats.begin(), lats.end());
    }

    if (!all_latencies.empty()) {
        std::sort(all_latencies.begin(), all_latencies.end());
        res.p50_latency_ns = all_latencies[all_latencies.size() * 50 / 100];
        res.p90_latency_ns = all_latencies[all_latencies.size() * 90 / 100];
        res.p99_latency_ns = all_latencies[all_latencies.size() * 99 / 100];
        res.p999_latency_ns = all_latencies[all_latencies.size() * 999 / 1000];
    }

    const size_t total_ops = all_latencies.size();
    if (res.total_time_ms > 0) {
        res.ops_per_sec = (total_ops / res.total_time_ms) * 1000.0;
        double total_bytes = total_ops * (options.key_size + options.val_size);
        res.mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (res.total_time_ms / 1000.0);
    }

    return res;
}

} // namespace lsm
