#include "atomic_batch.hpp"

namespace lsm {

namespace {

class MemTableInserter : public WriteBatch::Handler {
public:
    MemTableInserter(SequenceNumber seq, MemTableCore* mem)
        : current_seq_(seq), memtable_(mem) {}

    void Put(const Slice& key, const Slice& value) override {
        memtable_->Add(current_seq_, kTypeValue, key, value);
        current_seq_++;
    }

    void Delete(const Slice& key) override {
        memtable_->Add(current_seq_, kTypeDeletion, key, Slice(""));
        current_seq_++;
    }

private:
    SequenceNumber current_seq_{0};
    MemTableCore* memtable_{nullptr};
};

} // anonymous namespace

Status AtomicBatchCommitter::Commit(
    WriteBatch* batch,
    SequenceGenerator* seq_gen,
    MemTableCore* memtable,
    bool simulate_wal_failure
) {
    if (!batch || !seq_gen || !memtable) {
        return Status::InvalidArgument("Null components supplied to AtomicBatchCommitter");
    }

    uint32_t count = batch->Count();
    if (count == 0) {
        return Status::OK();
    }

    // Phase 1: Allocate contiguous sequence number window
    SequenceNumber start_seq = 0;
    seq_gen->BatchAllocate(count, &start_seq);
    batch->SetSequence(start_seq);

    // Phase 2: Atomic commit barrier (WAL write and fsync check)
    if (simulate_wal_failure) {
        // Abort: In the event of I/O failure, skip memtable ingestion entirely
        return Status::IOError("Simulated atomic WAL append failure: all-or-nothing rollback triggered");
    }

    // Phase 3: Apply full batch to MemTable atomically
    MemTableInserter inserter(start_seq, memtable);
    Status s = batch->Iterate(&inserter);
    if (!s.ok()) {
        return s;
    }

    return Status::OK();
}

} // namespace lsm
