#include "recovery.hpp"
#include "wal_reader.hpp"
#include "wal_format.hpp"
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>

namespace lsm {

namespace fs = std::filesystem;

std::string RecoveryReport::ToString() const {
    std::ostringstream oss;
    oss << "Recovery Report:\n"
        << "  Status:                 " << status.ToString() << "\n"
        << "  Files Scanned:          " << total_wal_files_scanned << "\n"
        << "  Records Replayed:       " << total_records_replayed << "\n"
        << "  Bytes Recovered:        " << total_bytes_recovered << " B\n"
        << "  Last Recovered LSN:     " << last_recovered_sequence << "\n"
        << "  Torn Write Truncated:   " << (torn_write_truncated ? "YES" : "NO") << "\n";
    if (torn_write_truncated) {
        oss << "  Truncated File Number:  " << truncated_file_number << "\n"
            << "  Truncated File Offset:  " << truncated_offset << " B\n";
    }
    return oss.str();
}

RecoveryEngine::RecoveryEngine(RecoveryOptions options)
    : options_(std::move(options)) {}

Status RecoveryEngine::ReplayBatch(
    const Slice& record_slice,
    MemTable* memtable,
    SequenceNumber* inout_last_seq
) {
    Slice payload = record_slice;
    std::string decrypted_buf;

    if (!options_.encryption_key.empty()) {
        uint8_t key[crypto::kKeySize];
        crypto::ChaCha20Poly1305::GenerateKeyFromSecret(options_.encryption_key, key);
        Status s = crypto::ChaCha20Poly1305::DecryptPayload(key, record_slice, &decrypted_buf);
        if (!s.ok()) {
            return s;
        }
        payload = Slice(decrypted_buf);
    }

    if (payload.size() < 12) {
        return Status::Corruption("WriteBatch header too small in recovery replay");
    }

    // Unpack WriteBatch from payload string
    WriteBatch batch;
    // Set raw rep_ in batch
    std::string* batch_rep = const_cast<std::string*>(&batch.Data());
    *batch_rep = payload.ToString();

    class RecoveryInserter : public WriteBatch::Handler {
    public:
        RecoveryInserter(SequenceNumber seq, MemTable* mem)
            : current_seq_(seq), mem_(mem) {}

        void Put(const Slice& key, const Slice& value) override {
            if (mem_) {
                mem_->Add(current_seq_, ValueType::kTypeValue, key, value);
            }
            current_seq_++;
        }

        void Delete(const Slice& key) override {
            if (mem_) {
                mem_->Add(current_seq_, ValueType::kTypeDeletion, key, Slice());
            }
            current_seq_++;
        }

        SequenceNumber NextSeq() const { return current_seq_; }

    private:
        SequenceNumber current_seq_{0};
        MemTable* mem_{nullptr};
    };

    SequenceNumber batch_seq = batch.Sequence();
    RecoveryInserter inserter(batch_seq, memtable);
    Status s = batch.Iterate(&inserter);
    if (s.ok()) {
        SequenceNumber max_in_batch = batch_seq + batch.Count() - 1;
        if (max_in_batch > *inout_last_seq) {
            *inout_last_seq = max_in_batch;
        }
    }
    return s;
}

Status RecoveryEngine::RecoverSingleFile(
    const std::string& log_file_path,
    uint64_t log_num,
    bool is_last_file,
    MemTable* memtable,
    SequenceNumber* inout_last_seq,
    RecoveryReport* report
) {
    int fd = open(log_file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status::IOError("Failed to open WAL for recovery: " + log_file_path);
    }

    auto seq_file = std::make_unique<PosixSequentialFile>(log_file_path, fd);
    WALReader reader(std::move(seq_file), nullptr, options_.verify_checksums);

    Slice record;
    std::string scratch;
    Status read_status = Status::OK();
    uint64_t last_valid_offset = 0;

    while (reader.ReadRecord(&record, &scratch, &read_status)) {
        Status s = ReplayBatch(record, memtable, inout_last_seq);
        if (!s.ok()) {
            return s;
        }
        if (report) {
            report->total_records_replayed++;
            report->total_bytes_recovered += record.size();
        }
        // Advance valid boundary to start of next record
        last_valid_offset = reader.LastRecordOffset() + wal::kHeaderSize + record.size();
        // If exact block boundary alignment
        if (last_valid_offset % wal::kBlockSize == 0 ||
            wal::kBlockSize - (last_valid_offset % wal::kBlockSize) < wal::kHeaderSize) {
            // Rounded to block boundary
            size_t rem = last_valid_offset % wal::kBlockSize;
            if (rem > 0) {
                last_valid_offset += (wal::kBlockSize - rem);
            }
        }
    }

    if (!read_status.ok()) {
        if (is_last_file && options_.truncate_corrupt_tail) {
            // Truncate the file at last valid physical offset
            uintmax_t file_len = fs::file_size(log_file_path);
            uint64_t target_len = std::min(static_cast<uint64_t>(file_len), last_valid_offset);
            if (truncate(log_file_path.c_str(), target_len) == 0) {
                if (report) {
                    report->torn_write_truncated = true;
                    report->truncated_file_number = log_num;
                    report->truncated_offset = target_len;
                }
                return Status::OK();
            } else {
                return Status::IOError("Failed to truncate torn write on log: " + log_file_path);
            }
        } else {
            return read_status;
        }
    }

    return Status::OK();
}

Status RecoveryEngine::Recover(
    const std::string& wal_dir,
    MemTable* memtable,
    SequenceNumber* out_last_seq,
    RecoveryReport* out_report
) {
    if (!out_last_seq) return Status::InvalidArgument("Null sequence output pointer");

    RecoveryReport report;
    *out_last_seq = 0;

    std::error_code ec;
    if (!fs::exists(wal_dir, ec)) {
        if (out_report) *out_report = report;
        return Status::OK(); // No WAL directory to recover
    }

    std::vector<std::pair<uint64_t, std::string>> log_files;
    for (const auto& entry : fs::directory_iterator(wal_dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".log") {
            try {
                uint64_t num = std::stoull(entry.path().stem().string());
                log_files.emplace_back(num, entry.path().string());
            } catch (...) {}
        }
    }

    std::sort(log_files.begin(), log_files.end());
    report.total_wal_files_scanned = log_files.size();

    SequenceNumber current_seq = 0;

    for (size_t i = 0; i < log_files.size(); ++i) {
        bool is_last = (i + 1 == log_files.size());
        Status s = RecoverSingleFile(
            log_files[i].second,
            log_files[i].first,
            is_last,
            memtable,
            &current_seq,
            &report
        );
        if (!s.ok()) {
            report.status = s;
            if (out_report) *out_report = report;
            return s;
        }
    }

    *out_last_seq = current_seq;
    report.last_recovered_sequence = current_seq;
    report.status = Status::OK();

    if (out_report) {
        *out_report = report;
    }

    return Status::OK();
}

} // namespace lsm
