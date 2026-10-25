#include "cf_write_pipeline.hpp"
#include <unordered_map>

namespace lsm {

namespace {

class MultiCFInserter : public CFWriteBatch::Handler {
public:
    MultiCFInserter(SequenceNumber start_seq, ColumnFamilySet* cf_set)
        : current_seq_(start_seq), cf_set_(cf_set) {}

    void Put(uint32_t cf_id, const Slice& key, const Slice& value) override {
        auto handle = cf_set_->GetColumnFamily(cf_id);
        if (handle && handle->cfd()) {
            handle->cfd()->Put(current_seq_, key, value);
        }
        current_seq_++;
    }

    void Delete(uint32_t cf_id, const Slice& key) override {
        auto handle = cf_set_->GetColumnFamily(cf_id);
        if (handle && handle->cfd()) {
            handle->cfd()->Delete(current_seq_, key);
        }
        current_seq_++;
    }

private:
    SequenceNumber current_seq_{0};
    ColumnFamilySet* cf_set_{nullptr};
};

} // anonymous namespace

Status CFWritePipeline::Write(
    CFWriteBatch* batch,
    ColumnFamilySet* cf_set,
    SequenceGenerator* seq_gen,
    bool simulate_wal_failure
) {
    if (!batch || !cf_set || !seq_gen) {
        return Status::InvalidArgument("Null components supplied to CFWritePipeline::Write");
    }

    uint32_t count = batch->Count();
    if (count == 0) {
        return Status::OK();
    }

    // Step 1: Sequence reservation
    SequenceNumber start_seq = 0;
    seq_gen->BatchAllocate(count, &start_seq);
    batch->SetSequence(start_seq);

    // Step 2: Atomic commit barrier
    if (simulate_wal_failure) {
        return Status::IOError("Simulated Cross-CF atomic WAL commit failure: batch aborted");
    }

    // Step 3: Demux and dispatch writes into per-CF independent memtables
    MultiCFInserter inserter(start_seq, cf_set);
    Status s = batch->Iterate(&inserter);
    if (!s.ok()) {
        return s;
    }

    return Status::OK();
}

} // namespace lsm
