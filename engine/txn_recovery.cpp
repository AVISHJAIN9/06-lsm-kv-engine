#include "txn_recovery.hpp"

namespace lsm {

namespace {

class MemTableReplayer : public WriteBatch::Handler {
public:
    MemTableReplayer(SequenceNumber seq, MemTableCore* mem)
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

Status TxnRecoveryManager::ReplayTransactions(
    const std::vector<TxnWALRecord>& wal_records,
    MemTableCore* memtable,
    SequenceGenerator* seq_gen,
    size_t* out_committed_replayed,
    size_t* out_uncommitted_discarded
) {
    if (!memtable || !seq_gen) {
        return Status::InvalidArgument("Null components supplied to TxnRecoveryManager");
    }

    size_t committed = 0;
    size_t discarded = 0;
    SequenceNumber max_seq = seq_gen->Last();

    for (const auto& rec : wal_records) {
        if (!rec.is_committed) {
            // Drop uncommitted transaction segment
            discarded++;
            continue;
        }

        uint32_t count = rec.batch.Count();
        if (count == 0) {
            committed++;
            continue;
        }

        SequenceNumber batch_seq = rec.seq;
        if (batch_seq == 0) {
            batch_seq = rec.batch.Sequence();
        }

        MemTableReplayer replayer(batch_seq, memtable);
        Status s = rec.batch.Iterate(&replayer);
        if (!s.ok()) {
            return s;
        }

        SequenceNumber end_seq = batch_seq + count - 1;
        if (end_seq > max_seq) {
            max_seq = end_seq;
        }
        committed++;
    }

    seq_gen->SetCurrent(max_seq);

    if (out_committed_replayed) *out_committed_replayed = committed;
    if (out_uncommitted_discarded) *out_uncommitted_discarded = discarded;

    return Status::OK();
}

} // namespace lsm
