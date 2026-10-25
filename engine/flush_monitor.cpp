#include "flush_monitor.hpp"

namespace lsm {

MemTableFlushMonitor::MemTableFlushMonitor(size_t write_buffer_size)
    : write_buffer_size_(write_buffer_size) {}

bool MemTableFlushMonitor::CheckAndTrigger(MemTableCore* memtable) {
    if (!memtable) return false;

    size_t usage = memtable->ApproximateMemoryUsage();
    size_t limit = write_buffer_size_.load(std::memory_order_relaxed);

    if (usage >= limit && !memtable->IsFlushPending()) {
        memtable->SetFlushPending(true);
        memtable->MarkImmutable();
        flush_signals_count_.fetch_add(1, std::memory_order_relaxed);

        if (callback_) {
            callback_(memtable, usage);
        }
        return true;
    }

    return false;
}

} // namespace lsm
