#include "audit_logger.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <openssl/sha.h>

namespace lsm {

namespace fs = std::filesystem;

std::string AuditLogger::ComputeSHA256(const std::string& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return oss.str();
}

std::string AuditRecord::ComputeHash() const {
    std::string payload = std::to_string(sequence) + "|" +
                          std::to_string(timestamp_micros) + "|" +
                          principal + "|" +
                          action + "|" +
                          details + "|" +
                          prev_record_hash;
    return AuditLogger::ComputeSHA256(payload);
}

std::string AuditRecord::Serialize() const {
    std::ostringstream oss;
    oss << sequence << "|"
        << timestamp_micros << "|"
        << principal << "|"
        << action << "|"
        << details << "|"
        << prev_record_hash << "|"
        << record_hash << "\n";
    return oss.str();
}

bool AuditRecord::Deserialize(const std::string& line, AuditRecord* out_rec) {
    if (line.empty()) return false;
    std::stringstream ss(line);
    std::string token;

    if (!std::getline(ss, token, '|')) return false;
    out_rec->sequence = std::stoull(token);

    if (!std::getline(ss, token, '|')) return false;
    out_rec->timestamp_micros = std::stoll(token);

    if (!std::getline(ss, token, '|')) return false;
    out_rec->principal = token;

    if (!std::getline(ss, token, '|')) return false;
    out_rec->action = token;

    if (!std::getline(ss, token, '|')) return false;
    out_rec->details = token;

    if (!std::getline(ss, token, '|')) return false;
    out_rec->prev_record_hash = token;

    if (!std::getline(ss, token)) return false;
    out_rec->record_hash = token;

    return true;
}

AuditLogger::AuditLogger(std::string log_file_path)
    : log_file_path_(std::move(log_file_path)) {
    InitializeLog();
}

void AuditLogger::InitializeLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream in(log_file_path_);
    if (!in.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        AuditRecord rec;
        if (AuditRecord::Deserialize(line, &rec)) {
            next_seq_ = rec.sequence + 1;
            last_hash_ = rec.record_hash;
        }
    }
}

Status AuditLogger::LogEvent(
    const std::string& principal,
    const std::string& action,
    const std::string& details,
    AuditRecord* out_rec
) {
    std::lock_guard<std::mutex> lock(mutex_);

    AuditRecord rec;
    rec.sequence = next_seq_++;
    rec.timestamp_micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    rec.principal = principal;
    rec.action = action;
    rec.details = details;
    rec.prev_record_hash = last_hash_;
    rec.record_hash = rec.ComputeHash();

    last_hash_ = rec.record_hash;

    // Append record to log file
    std::ofstream out(log_file_path_, std::ios::app);
    if (!out.is_open()) {
        return Status::IOError("Failed to open audit log file for appending: " + log_file_path_);
    }
    out << rec.Serialize();
    out.flush();
    out.close();

    if (out_rec) *out_rec = rec;
    return Status::OK();
}

Status AuditLogger::VerifyLogIntegrity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream in(log_file_path_);
    if (!in.is_open()) {
        return Status::OK(); // Empty log is trivially valid
    }

    std::string line;
    std::string expected_prev_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    uint64_t expected_seq = 1;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        AuditRecord rec;
        if (!AuditRecord::Deserialize(line, &rec)) {
            return Status::Corruption("Malformed audit record line in log");
        }

        if (rec.sequence != expected_seq) {
            return Status::Corruption("Sequence number discontinuity at sequence " + std::to_string(rec.sequence) +
                                      " (expected " + std::to_string(expected_seq) + ")");
        }

        if (rec.prev_record_hash != expected_prev_hash) {
            return Status::Corruption("Broken hash chain at sequence " + std::to_string(rec.sequence) +
                                      ": prev_hash mismatch (expected " + expected_prev_hash +
                                      ", got " + rec.prev_record_hash + ")");
        }

        std::string computed_hash = rec.ComputeHash();
        if (computed_hash != rec.record_hash) {
            return Status::Corruption("Tampered record detected at sequence " + std::to_string(rec.sequence) +
                                      ": hash mismatch (expected " + rec.record_hash +
                                      ", computed " + computed_hash + ")");
        }

        expected_prev_hash = rec.record_hash;
        expected_seq++;
    }

    return Status::OK();
}

size_t AuditLogger::GetRecordCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_seq_ - 1;
}

std::string AuditLogger::GetLastHash() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_hash_;
}

} // namespace lsm
