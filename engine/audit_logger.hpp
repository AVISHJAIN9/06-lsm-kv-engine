#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include "status.hpp"

namespace lsm {

struct AuditRecord {
    uint64_t sequence{0};
    int64_t timestamp_micros{0};
    std::string principal;
    std::string action;
    std::string details;
    std::string prev_record_hash;
    std::string record_hash;

    std::string Serialize() const;
    static bool Deserialize(const std::string& line, AuditRecord* out_rec);
    std::string ComputeHash() const;
};

// AuditLogger: Cryptographically secure SHA-256 Merkle hash-chained tamper-evident audit ledger (Feature 141)
class AuditLogger {
public:
    explicit AuditLogger(std::string log_file_path);
    ~AuditLogger() = default;

    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    // Appends an audit record to the tamper-evident ledger and computes the next hash link
    Status LogEvent(
        const std::string& principal,
        const std::string& action,
        const std::string& details,
        AuditRecord* out_rec = nullptr
    );

    // Scans the entire audit log from genesis to tip, verifying mathematical cryptographic integrity
    Status VerifyLogIntegrity() const;

    size_t GetRecordCount() const;
    std::string GetLastHash() const;
    std::string GetLogFilePath() const { return log_file_path_; }

    static std::string ComputeSHA256(const std::string& data);

private:
    std::string log_file_path_;
    mutable std::mutex mutex_;
    uint64_t next_seq_{1};
    std::string last_hash_{"0000000000000000000000000000000000000000000000000000000000000000"};

    void InitializeLog();
};

} // namespace lsm
