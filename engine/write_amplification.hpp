#pragma once

#include <cstdint>
#include <atomic>
#include <string>

namespace lsm {

struct WAFSummary {
    uint64_t bytes_user_written{0};
    uint64_t bytes_wal_written{0};
    uint64_t bytes_flush_written{0};
    uint64_t bytes_compaction_written{0};
    uint64_t total_physical_bytes_written{0};
    double cumulative_waf{1.0};
    double compaction_waf{0.0};

    std::string ToString() const;
};

// -----------------------------------------------------------------------------
// WriteAmplificationTracker: Real-time atomic accounting of user vs physical disk I/O
// -----------------------------------------------------------------------------
class WriteAmplificationTracker {
public:
    WriteAmplificationTracker() = default;

    void RecordUserWrite(size_t bytes) {
        bytes_user_written_.fetch_add(bytes, std::memory_order_relaxed);
    }

    void RecordWALWrite(size_t bytes) {
        bytes_wal_written_.fetch_add(bytes, std::memory_order_relaxed);
    }

    void RecordFlushWrite(size_t bytes) {
        bytes_flush_written_.fetch_add(bytes, std::memory_order_relaxed);
    }

    void RecordCompactionWrite(size_t bytes) {
        bytes_compaction_written_.fetch_add(bytes, std::memory_order_relaxed);
    }

    double GetCumulativeWAF() const {
        uint64_t user = bytes_user_written_.load(std::memory_order_relaxed);
        if (user == 0) return 1.0;
        uint64_t physical = bytes_wal_written_.load(std::memory_order_relaxed) +
                            bytes_flush_written_.load(std::memory_order_relaxed) +
                            bytes_compaction_written_.load(std::memory_order_relaxed);
        return static_cast<double>(physical) / user;
    }

    double GetCompactionWAF() const {
        uint64_t user = bytes_user_written_.load(std::memory_order_relaxed);
        if (user == 0) return 0.0;
        uint64_t compaction = bytes_compaction_written_.load(std::memory_order_relaxed);
        return static_cast<double>(compaction) / user;
    }

    WAFSummary GetSummary() const;

    void Reset() {
        bytes_user_written_.store(0, std::memory_order_relaxed);
        bytes_wal_written_.store(0, std::memory_order_relaxed);
        bytes_flush_written_.store(0, std::memory_order_relaxed);
        bytes_compaction_written_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> bytes_user_written_{0};
    std::atomic<uint64_t> bytes_wal_written_{0};
    std::atomic<uint64_t> bytes_flush_written_{0};
    std::atomic<uint64_t> bytes_compaction_written_{0};
};

} // namespace lsm
