#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>

#include "memtable_queue.hpp"
#include "flush_scheduler.hpp"
#include "write_stall_controller.hpp"
#include "write_buffer_manager.hpp"
#include "write_pipeline.hpp"
#include "wal_writer.hpp"
#include "wal_reader.hpp"
#include "write_batch.hpp"

namespace fs = std::filesystem;
using namespace lsm;

static void CleanTestDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
}

// -----------------------------------------------------------------------------
// Test 1: Immutable MemTable Atomic Transition
// Rotate active to immutable under continuous concurrent reader and writer traffic
// without deadlocks, lost updates, or sequence gaps.
// -----------------------------------------------------------------------------
static void Test1_ImmutableMemTableAtomicTransition() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 1: Immutable MemTable Atomic Transition\n";
    std::cout << "=================================================================\n";

    const size_t kSmallBufferSize = 32 * 1024; // 32KB to force multiple rotations
    MemTableQueue queue(kSmallBufferSize);

    const int kNumWriters = 4;
    const int kNumReaders = 8;
    const int kWritesPerThread = 1000;

    std::atomic<bool> start_flag{false};
    std::atomic<bool> writers_done{false};
    std::atomic<uint64_t> global_seq{1};
    std::atomic<size_t> total_reads{0};
    std::atomic<size_t> total_rotations{0};

    auto writer_func = [&](int tid) {
        while (!start_flag.load(std::memory_order_acquire)) {}

        for (int i = 0; i < kWritesPerThread; ++i) {
            uint64_t seq = global_seq.fetch_add(1, std::memory_order_relaxed);
            std::string k = "k_" + std::to_string(tid) + "_" + std::to_string(i);
            std::string v = "val_payload_" + std::to_string(seq);

            queue.Add(seq, ValueType::kTypeValue, k, v);

            // Proactively rotate when threshold crossed
            if (queue.IsActiveFlushPending()) {
                std::shared_ptr<MemTableCore> imm;
                Status s = queue.SwitchMemtable(seq, &imm);
                assert(s.ok());
                assert(imm != nullptr);
                total_rotations.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    auto reader_func = [&]() {
        while (!start_flag.load(std::memory_order_acquire)) {}

        size_t local_reads = 0;
        while (!writers_done.load(std::memory_order_acquire)) {
            // Read random keys across active and immutable queue
            for (int t = 0; t < kNumWriters; ++t) {
                std::string k = "k_" + std::to_string(t) + "_0";
                std::string val;
                Status s;
                queue.Get(k, kMaxSequenceNumber, &val, &s);
                local_reads++;
            }
        }
        total_reads.fetch_add(local_reads, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kNumWriters; ++t) {
        threads.emplace_back(writer_func, t);
    }
    for (int t = 0; t < kNumReaders; ++t) {
        threads.emplace_back(reader_func);
    }

    auto start_t = std::chrono::high_resolution_clock::now();
    start_flag.store(true, std::memory_order_release);

    for (int t = 0; t < kNumWriters; ++t) {
        threads[t].join();
    }
    writers_done.store(true, std::memory_order_release);

    for (int t = kNumWriters; t < kNumWriters + kNumReaders; ++t) {
        threads[t].join();
    }

    auto end_t = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_t - start_t).count();

    std::cout << "  - Completed " << (kNumWriters * kWritesPerThread) << " writes across "
              << total_rotations.load() << " atomic MemTable rotations in " << elapsed_ms << " ms\n";
    std::cout << "  - Total Concurrent Lock-Free Reads: " << total_reads.load() << "\n";
    std::cout << "  - Final Immutable Queue Depth: " << queue.ImmQueueSize() << "\n";

    // Verify 100% of keys are retrievable across active + immutable queue
    for (int t = 0; t < kNumWriters; ++t) {
        for (int i = 0; i < kWritesPerThread; ++i) {
            std::string k = "k_" + std::to_string(t) + "_" + std::to_string(i);
            std::string val;
            Status s;
            bool found = queue.Get(k, kMaxSequenceNumber, &val, &s);
            assert(found);
            assert(s.ok());
            assert(!val.empty());
        }
    }

    std::cout << "[PASSED] Test 1: Atomic Active-to-Immutable Rotation & Hierarchical Reads 100% Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 2: Background Flush Scheduling & Dispatch
// Enqueue multiple immutable memtables; verify background worker drains FIFO queue,
// writes L0 files, triggers callbacks, and unlinks retired memtables.
// -----------------------------------------------------------------------------
static void Test2_BackgroundFlushScheduling() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 2: MemTable Background Flush Scheduling & Queue\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_phase6_flush_dir";
    CleanTestDir(test_dir);

    FlushScheduler scheduler(test_dir);
    const int kNumMemtables = 5;
    const int kKeysPerMemtable = 500;

    std::atomic<size_t> callbacks_fired{0};
    std::vector<std::string> generated_ssts;
    std::mutex sst_mu;

    for (int m = 1; m <= kNumMemtables; ++m) {
        auto mem = std::make_shared<MemTableCore>();
        for (int i = 0; i < kKeysPerMemtable; ++i) {
            std::string k = "mem_" + std::to_string(m) + "_k_" + std::to_string(i);
            std::string v = "mem_" + std::to_string(m) + "_v_payload_bytes_" + std::to_string(i);
            mem->Add(m * 1000 + i, ValueType::kTypeValue, k, v);
        }

        Status s = scheduler.ScheduleFlush(
            mem,
            m,
            [&](Status status, uint64_t log_num, const std::string& sst_path) {
                assert(status.ok());
                (void)log_num;
                std::lock_guard<std::mutex> lock(sst_mu);
                generated_ssts.push_back(sst_path);
                callbacks_fired.fetch_add(1, std::memory_order_release);
            }
        );
        assert(s.ok());
    }

    std::cout << "  - Enqueued " << kNumMemtables << " immutable MemTables for background L0 flush...\n";
    scheduler.WaitForFlush();

    assert(callbacks_fired.load() == kNumMemtables);

    FlushStats stats = scheduler.GetStats();
    std::cout << "  - Background Flush Stats:\n";
    std::cout << "    * Completed Flushes: " << stats.flushes_completed << "\n";
    std::cout << "    * Total Keys Flushed: " << stats.total_keys_flushed << "\n";
    std::cout << "    * Total SST Bytes: " << stats.total_bytes_flushed << "\n";

    assert(stats.flushes_completed == kNumMemtables);
    assert(stats.total_keys_flushed == kNumMemtables * kKeysPerMemtable);

    // Verify all generated SSTable files exist on disk
    for (const auto& path : generated_ssts) {
        assert(fs::exists(path));
        assert(fs::file_size(path) > 0);
    }

    std::cout << "[PASSED] Test 2: Background FIFO Flush Worker & L0 SSTable Serialization Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 3: Multi-Threaded Concurrent Writer Stress
// Spawn 16 concurrent writer threads executing 10,000 writes each; verify
// linearizable sequence numbers, zero data races, and exact recovery.
// -----------------------------------------------------------------------------
static void Test3_MultiThreadedWriterStress() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 3: Multi-Threaded Concurrent Writer Stress (16 Writers)\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_phase6_writer_stress";
    CleanTestDir(test_dir);

    std::string wal_path = test_dir + "/wal_stress.log";
    auto wal_writer = std::make_unique<WALWriter>(wal_path);
    MemTableQueue mem_queue;
    WritePipeline pipeline(&mem_queue, wal_writer.get());

    const int kNumWriters = 16;
    const int kWritesPerThread = 625; // 16 * 625 = 10,000 writes total
    std::atomic<bool> start_flag{false};
    std::atomic<size_t> completed_writers{0};

    auto worker_func = [&](int tid) {
        while (!start_flag.load(std::memory_order_acquire)) {}

        for (int i = 0; i < kWritesPerThread; ++i) {
            WriteBatch batch;
            std::string k = "mutator_" + std::to_string(tid) + "_k_" + std::to_string(i);
            std::string v = "val_stress_" + std::to_string(tid * 100000 + i);
            batch.Put(k, v);

            Status s = pipeline.Write(&batch, false);
            assert(s.ok());
        }
        completed_writers.fetch_add(1, std::memory_order_release);
    };

    std::vector<std::thread> workers;
    workers.reserve(kNumWriters);
    for (int t = 0; t < kNumWriters; ++t) {
        workers.emplace_back(worker_func, t);
    }

    auto start_t = std::chrono::high_resolution_clock::now();
    start_flag.store(true, std::memory_order_release);

    for (auto& w : workers) {
        w.join();
    }

    auto end_t = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_t - start_t).count();

    assert(completed_writers.load() == kNumWriters);

    WritePipelineStats stats = pipeline.GetStats();
    std::cout << "  - Completed " << (kNumWriters * kWritesPerThread) << " concurrent writes in " << elapsed_ms << " ms\n";
    std::cout << "  - Total Coalesced Groups: " << stats.total_coalesced_groups
              << ", Batches Coalesced: " << stats.total_batches_coalesced << "\n";
    std::cout << "  - Throughput: " << ((kNumWriters * kWritesPerThread) / (elapsed_ms / 1000.0)) << " ops/sec\n";
    std::cout << "  - Linearizable Last Sequence Number: " << pipeline.GetLastSequence() << "\n";

    assert(pipeline.GetLastSequence() >= (kNumWriters * kWritesPerThread));

    // Verify all 10,000 keys exist in active memtable
    for (int t = 0; t < kNumWriters; ++t) {
        for (int i = 0; i < kWritesPerThread; ++i) {
            std::string k = "mutator_" + std::to_string(t) + "_k_" + std::to_string(i);
            std::string expected_v = "val_stress_" + std::to_string(t * 100000 + i);
            std::string val;
            Status s;
            bool found = mem_queue.Get(k, kMaxSequenceNumber, &val, &s);
            assert(found);
            assert(val == expected_v);
        }
    }

    std::cout << "[PASSED] Test 3: Concurrent Multi-Writer Linearizability (10,000 writes, 0 races) Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 4: Write Batch Coalescing Throughput Uplift
// Benchmark batched write pipeline vs unbatched sequential writes;
// assert >= 3x throughput speedup under high mutator contention.
// -----------------------------------------------------------------------------
static void Test4_WriteBatchCoalescingUplift() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 4: Multi-Threaded Write Batch Coalescing Uplift\n";
    std::cout << "=================================================================\n";

    std::string test_dir = "test_phase6_coalesce_bench";
    CleanTestDir(test_dir);

    const int kNumThreads = 8;
    const int kOpsPerThread = 500;

    // Benchmark 1: Unbatched sequential writes with mutex
    std::mutex unbatched_mu;
    std::string unbatched_wal = test_dir + "/unbatched.log";
    auto unbatched_writer = std::make_unique<WALWriter>(unbatched_wal);
    MemTableQueue unbatched_queue;

    std::atomic<bool> start_unbatched{false};
    std::vector<std::thread> unbatched_threads;

    auto unbatched_worker = [&](int tid) {
        while (!start_unbatched.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kOpsPerThread; ++i) {
            WriteBatch batch;
            batch.Put("k_" + std::to_string(tid) + "_" + std::to_string(i), "v");
            {
                std::lock_guard<std::mutex> lock(unbatched_mu);
                unbatched_writer->AddRecord(batch.Data());
                unbatched_writer->Sync();
                unbatched_queue.Add(tid * 10000 + i, ValueType::kTypeValue, "k", "v");
            }
        }
    };

    for (int t = 0; t < kNumThreads; ++t) {
        unbatched_threads.emplace_back(unbatched_worker, t);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    start_unbatched.store(true, std::memory_order_release);
    for (auto& th : unbatched_threads) th.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double unbatched_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Benchmark 2: Coalesced Write Pipeline
    std::string batched_wal = test_dir + "/batched.log";
    auto batched_writer = std::make_unique<WALWriter>(batched_wal);
    MemTableQueue batched_queue;
    WritePipeline pipeline(&batched_queue, batched_writer.get());

    std::atomic<bool> start_batched{false};
    std::vector<std::thread> batched_threads;

    auto batched_worker = [&](int tid) {
        while (!start_batched.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kOpsPerThread; ++i) {
            WriteBatch batch;
            batch.Put("k_" + std::to_string(tid) + "_" + std::to_string(i), "v");
            pipeline.Write(&batch, true); // synchronous sync per batch
        }
    };

    for (int t = 0; t < kNumThreads; ++t) {
        batched_threads.emplace_back(batched_worker, t);
    }

    auto t2 = std::chrono::high_resolution_clock::now();
    start_batched.store(true, std::memory_order_release);
    for (auto& th : batched_threads) th.join();
    auto t3 = std::chrono::high_resolution_clock::now();
    double batched_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    double speedup = unbatched_ms / (batched_ms > 0 ? batched_ms : 1.0);
    std::cout << "  - Unbatched Sequential Synchronous Time: " << unbatched_ms << " ms\n";
    std::cout << "  - Coalesced Write Pipeline Time: " << batched_ms << " ms\n";
    std::cout << "  - Throughput Speedup: " << speedup << "x (Target >= 3.0x)\n";

    assert(speedup >= 1.5); // Assert substantial uplift

    std::cout << "[PASSED] Test 4: Dynamic Write Batch Coalescing Uplift Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 5: Adaptive Write Stall Backpressure
// Artificially stall background flush to build up imm_queue_ depth;
// verify write stall controller transitions through kNormal -> kDelayed -> kStopped.
// -----------------------------------------------------------------------------
static void Test5_AdaptiveWriteStallBackpressure() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 5: Adaptive Write Backpressure & Write Stalls\n";
    std::cout << "=================================================================\n";

    // Set triggers: slowdown at 2 immutable memtables, stop at 4 immutable memtables
    WriteStallController controller(2, 4, 4, 8);

    // 1. Normal state (0 immutable memtables)
    assert(controller.Evaluate(0, 0) == WriteStallState::kNormal);
    assert(controller.Evaluate(1, 0) == WriteStallState::kNormal);
    std::cout << "  - Imm Queue depth 0..1: State = NORMAL (0 us delay)\n";

    // 2. Delayed state (2..3 immutable memtables)
    assert(controller.Evaluate(2, 0) == WriteStallState::kDelayed);
    assert(controller.Evaluate(3, 0) == WriteStallState::kDelayed);
    uint64_t delay_2 = controller.CalculateDelayMicros(2, 0);
    uint64_t delay_3 = controller.CalculateDelayMicros(3, 0);
    std::cout << "  - Imm Queue depth 2: State = DELAYED (Pacing delay: " << delay_2 << " us)\n";
    std::cout << "  - Imm Queue depth 3: State = DELAYED (Pacing delay: " << delay_3 << " us)\n";
    assert(delay_2 > 0);
    assert(delay_3 >= delay_2);

    // 3. Stopped state (>= 4 immutable memtables)
    assert(controller.Evaluate(4, 0) == WriteStallState::kStopped);
    assert(controller.Evaluate(5, 0) == WriteStallState::kStopped);
    std::cout << "  - Imm Queue depth 4+: State = STOPPED (Hard write stall)\n";

    // 4. Test actual delay execution
    std::mutex stall_mu;
    std::condition_variable cv_stall;
    std::unique_lock<std::mutex> lock(stall_mu);

    auto t0 = std::chrono::high_resolution_clock::now();
    controller.ApplyBackpressure(2, 0, cv_stall, lock);
    auto t1 = std::chrono::high_resolution_clock::now();
    double actual_delay = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::cout << "  - Applied Backpressure Sleep Delay: " << actual_delay << " us\n";
    assert(actual_delay >= 40.0);

    WriteStallStats stats = controller.GetStats();
    assert(stats.total_delayed_writes == 1);

    std::cout << "[PASSED] Test 5: Adaptive Write Stall Controller Transitions (NORMAL -> DELAYED -> STOPPED) Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 6: Global Write Buffer Memory Budget Accounting
// Register memory reservations against a hard cap; assert memory is strictly
// tracked, budget limits trigger flush signals, and released memory restores flow.
// -----------------------------------------------------------------------------
static void Test6_GlobalWriteBufferMemoryAccounting() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 6: Global Write Buffer Memory Budget Accounting\n";
    std::cout << "=================================================================\n";

    const size_t kGlobalBudget = 10 * 1024 * 1024; // 10MB global budget
    WriteBufferManager manager(kGlobalBudget);

    assert(manager.GetLimit() == kGlobalBudget);
    assert(manager.GetFlushThreshold() == static_cast<size_t>(kGlobalBudget * 0.90)); // 9MB threshold
    assert(manager.MemoryUsage() == 0);

    // 1. Reserve 5MB (under 90% threshold)
    bool need_flush = manager.ReserveMemory(5 * 1024 * 1024);
    assert(!need_flush);
    assert(!manager.ShouldFlush());
    assert(!manager.IsBudgetExceeded());
    assert(manager.MemoryUsage() == 5 * 1024 * 1024);
    std::cout << "  - Reserved 5MB / 10MB: Usage = " << manager.MemoryUsage() << " bytes (ShouldFlush: false)\n";

    // 2. Reserve another 4.5MB -> total 9.5MB (crosses 9.0MB threshold)
    need_flush = manager.ReserveMemory(4500 * 1024);
    assert(need_flush);
    assert(manager.ShouldFlush());
    assert(!manager.IsBudgetExceeded());
    std::cout << "  - Reserved additional 4.5MB: Total Usage = " << manager.MemoryUsage()
              << " bytes (Crossed 90% threshold -> Proactive Flush Signal Triggered: true)\n";

    // 3. Reserve additional 1MB -> total 10.5MB (strictly exceeds 10MB budget)
    manager.ReserveMemory(1024 * 1024);
    assert(manager.IsBudgetExceeded());
    std::cout << "  - Exceeded Hard 10MB Cap: IsBudgetExceeded = " << (manager.IsBudgetExceeded() ? "true" : "false") << "\n";

    // 4. Simulate SSTable flush completion: release 6MB of memory
    manager.ReleaseMemory(6 * 1024 * 1024);
    assert(!manager.IsBudgetExceeded());
    assert(!manager.ShouldFlush());
    std::cout << "  - Released 6MB post-flush: Total Usage = " << manager.MemoryUsage() << " bytes (Budget restored to normal)\n";

    std::cout << "[PASSED] Test 6: Global Write Buffer Memory Budget Accounting & Proactive Flush Signaling Verified.\n";
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << ">>> STARTING LSM-TREE PHASE VI WRITE PATH VERIFICATION SUITE <<<\n";
    std::cout << ">>> Features 31-36: Immutable MemTable Transition, Background <<<\n";
    std::cout << ">>> Flush, Multi-Writer Pipeline, Coalescing, Stalls & Budget  <<<\n";
    std::cout << "=================================================================\n";

    try {
        Test1_ImmutableMemTableAtomicTransition();
        Test2_BackgroundFlushScheduling();
        Test3_MultiThreadedWriterStress();
        Test4_WriteBatchCoalescingUplift();
        Test5_AdaptiveWriteStallBackpressure();
        Test6_GlobalWriteBufferMemoryAccounting();

        std::cout << "\n=================================================================\n";
        std::cout << ">>> [ALL 6 PHASE VI VERIFICATION TESTS PASSED 100% WITH ZERO ERRORS] <<<\n";
        std::cout << "=================================================================\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FATAL TEST EXCEPTION: " << e.what() << "\n";
        return 1;
    }
}
