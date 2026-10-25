#include "memtable_recovery.hpp"
#include "wal_reader.hpp"
#include <filesystem>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <regex>

namespace lsm {

namespace fs = std::filesystem;

namespace {

struct LogFileMetadata {
    uint64_t log_number;
    std::string path;

    bool operator<(const LogFileMetadata& other) const {
        return log_number < other.log_number;
    }
};

class QueueInserter : public WriteBatch::Handler {
public:
    QueueInserter(SequenceNumber seq, MemTableQueue* queue)
        : current_seq_(seq), queue_(queue) {}

    void Put(const Slice& key, const Slice& value) override {
        if (queue_) {
            queue_->Add(current_seq_, ValueType::kTypeValue, key, value);
        }
        current_seq_++;
    }

    void Delete(const Slice& key) override {
        if (queue_) {
            queue_->Add(current_seq_, ValueType::kTypeDeletion, key, Slice());
        }
        current_seq_++;
    }

    SequenceNumber NextSeq() const { return current_seq_; }

private:
    SequenceNumber current_seq_{0};
    MemTableQueue* queue_{nullptr};
};

} // namespace

MemTableRecoveryCoordinator::MemTableRecoveryCoordinator(std::string db_dir)
    : db_dir_(std::move(db_dir)) {}

Status MemTableRecoveryCoordinator::Recover(
    uint64_t min_log_number,
    MemTableQueue* queue,
    MultiWalRecoveryStats* stats
) {
    if (!queue) {
        return Status::InvalidArgument("Null MemTableQueue passed to recovery");
    }

    std::vector<LogFileMetadata> log_files;
    std::error_code ec;

    if (!fs::exists(db_dir_, ec)) {
        return Status::NotFound("Database directory does not exist: " + db_dir_);
    }

    for (const auto& entry : fs::directory_iterator(db_dir_, ec)) {
        if (!entry.is_regular_file()) continue;

        std::string filename = entry.path().filename().string();
        if (entry.path().extension() == ".log") {
            // Match patterns like "000001.log", "wal_1.log", "1.log"
            uint64_t log_num = 0;
            std::string stem = entry.path().stem().string();
            if (stem.rfind("wal_", 0) == 0) {
                stem = stem.substr(4);
            }
            try {
                log_num = std::stoull(stem);
            } catch (...) {
                continue;
            }

            if (log_num >= min_log_number) {
                log_files.push_back({log_num, entry.path().string()});
            }
        }
    }

    std::sort(log_files.begin(), log_files.end());

    if (stats) {
        stats->total_wal_files_scanned = log_files.size();
    }

    uint64_t max_seq = 0;

    for (const auto& log_file : log_files) {
        int fd = open(log_file.path.c_str(), O_RDONLY);
        if (fd < 0) {
            return Status::IOError("Failed to open WAL file: " + log_file.path);
        }

        auto seq_file = std::make_unique<PosixSequentialFile>(log_file.path, fd);
        WALReader reader(std::move(seq_file), nullptr, true);

        Slice record;
        std::string scratch;
        Status read_status = Status::OK();

        while (reader.ReadRecord(&record, &scratch, &read_status)) {
            if (record.size() < 12) {
                return Status::Corruption("Corrupt WAL record payload: header too small");
            }

            WriteBatch batch;
            std::string* batch_rep = const_cast<std::string*>(&batch.Data());
            *batch_rep = record.ToString();

            SequenceNumber batch_seq = batch.Sequence();
            QueueInserter inserter(batch_seq, queue);
            Status s = batch.Iterate(&inserter);
            if (!s.ok()) {
                return s;
            }

            if (batch.Count() > 0) {
                SequenceNumber highest_in_batch = batch_seq + batch.Count() - 1;
                if (highest_in_batch > max_seq) {
                    max_seq = highest_in_batch;
                }
            }

            if (stats) {
                stats->total_records_replayed += batch.Count();
                stats->total_batches_replayed++;
                stats->total_bytes_recovered += record.size();
            }

            // Proactively switch memtable during recovery if write buffer filled
            if (queue->IsActiveFlushPending()) {
                std::shared_ptr<MemTableCore> imm;
                Status switch_status = queue->SwitchMemtable(log_file.log_number, &imm);
                if (!switch_status.ok()) {
                    return switch_status;
                }
                if (stats) {
                    stats->memtable_switches++;
                }
            }
        }

        if (!read_status.ok() && !read_status.IsNotFound()) {
            return read_status;
        }
    }

    if (stats) {
        stats->last_recovered_seq = max_seq;
    }

    return Status::OK();
}

} // namespace lsm
