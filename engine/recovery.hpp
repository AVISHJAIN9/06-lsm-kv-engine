#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "slice.hpp"
#include "status.hpp"
#include "types.hpp"
#include "memtable.hpp"
#include "write_batch.hpp"
#include "wal_crypto.hpp"

namespace lsm {

struct RecoveryOptions {
    bool truncate_corrupt_tail{true};
    bool verify_checksums{true};
    std::string encryption_key{""};
};

struct RecoveryReport {
    size_t total_wal_files_scanned{0};
    size_t total_records_replayed{0};
    size_t total_bytes_recovered{0};
    SequenceNumber last_recovered_sequence{0};
    bool torn_write_truncated{false};
    uint64_t truncated_file_number{0};
    uint64_t truncated_offset{0};
    Status status{Status::OK()};

    std::string ToString() const;
};

class RecoveryEngine {
public:
    explicit RecoveryEngine(RecoveryOptions options = RecoveryOptions());
    ~RecoveryEngine() = default;

    Status Recover(
        const std::string& wal_dir,
        MemTable* memtable,
        SequenceNumber* out_last_seq,
        RecoveryReport* out_report = nullptr
    );

    Status RecoverSingleFile(
        const std::string& log_file_path,
        uint64_t log_num,
        bool is_last_file,
        MemTable* memtable,
        SequenceNumber* inout_last_seq,
        RecoveryReport* report
    );

private:
    RecoveryOptions options_{};

    Status ReplayBatch(
        const Slice& record_slice,
        MemTable* memtable,
        SequenceNumber* inout_last_seq
    );
};

} // namespace lsm
