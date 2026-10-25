#include "pitr_recovery.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace lsm {

namespace fs = std::filesystem;

static bool CopyDirectoryContents(const std::string& src_dir, const std::string& dst_dir) {
    std::error_code ec;
    fs::create_directories(dst_dir, ec);
    if (ec) return false;

    if (fs::exists(src_dir)) {
        for (const auto& entry : fs::directory_iterator(src_dir)) {
            std::string dest_file = (fs::path(dst_dir) / entry.path().filename()).string();
            fs::copy_file(entry.path(), dest_file, fs::copy_options::overwrite_existing, ec);
        }
    }
    return true;
}

std::string PITRLogRecord::Serialize() const {
    std::ostringstream oss;
    oss << sequence << "|"
        << timestamp_micros << "|"
        << (type == ValueType::kTypeValue ? "PUT" : "DEL") << "|"
        << key << "|"
        << value << "\n";
    return oss.str();
}

bool PITRLogRecord::Deserialize(const std::string& line, PITRLogRecord* out_rec) {
    if (line.empty()) return false;
    std::stringstream ss(line);
    std::string token;

    if (!std::getline(ss, token, '|')) return false;
    out_rec->sequence = std::stoull(token);

    if (!std::getline(ss, token, '|')) return false;
    out_rec->timestamp_micros = std::stoll(token);

    if (!std::getline(ss, token, '|')) return false;
    out_rec->type = (token == "DEL" ? ValueType::kTypeDeletion : ValueType::kTypeValue);

    if (!std::getline(ss, token, '|')) return false;
    out_rec->key = token;

    // Remaining line is value (can be empty for DEL)
    std::getline(ss, token);
    out_rec->value = token;

    return true;
}

Status PITRCoordinator::RecoverToTimestamp(
    int64_t target_epoch_micros,
    const std::string& base_backup_dir,
    const std::vector<std::string>& wal_archive_files,
    const std::string& target_restore_dir,
    std::shared_ptr<MemTableCore>& out_memtable,
    PITRReplaySummary* out_summary
) {
    // 1. Restore base backup physical files
    if (!base_backup_dir.empty()) {
        if (!CopyDirectoryContents(base_backup_dir, target_restore_dir)) {
            return Status::IOError("Failed to restore base backup files to target directory");
        }
    }

    if (!out_memtable) {
        out_memtable = std::make_shared<MemTableCore>();
    }

    PITRReplaySummary summary;
    bool cutoff_reached = false;

    // 2. Sequentially scan and replay WAL archives
    for (const auto& wal_path : wal_archive_files) {
        std::ifstream in(wal_path, std::ios::binary);
        if (!in.is_open()) {
            continue;
        }

        // Check if file is compressed archive (.arc) with header [RAW_SIZE 8B][CRC 4B]
        std::string raw_content;
        if (wal_path.size() >= 4 && wal_path.substr(wal_path.size() - 4) == ".arc") {
            uint64_t raw_size = 0;
            uint32_t orig_crc = 0;
            in.read(reinterpret_cast<char*>(&raw_size), sizeof(raw_size));
            in.read(reinterpret_cast<char*>(&orig_crc), sizeof(orig_crc));
            raw_content.resize(raw_size);
            in.read(raw_content.data(), raw_size);
        } else {
            raw_content = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        in.close();

        std::stringstream ss(raw_content);
        std::string line;

        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            PITRLogRecord rec;
            if (!PITRLogRecord::Deserialize(line, &rec)) {
                continue;
            }

            summary.records_scanned++;
            if (summary.earliest_timestamp_micros == 0 || rec.timestamp_micros < summary.earliest_timestamp_micros) {
                summary.earliest_timestamp_micros = rec.timestamp_micros;
            }

            if (rec.timestamp_micros > target_epoch_micros) {
                // Precise microsecond cutoff point reached!
                summary.records_skipped_after_cutoff++;
                cutoff_reached = true;
                break;
            }

            // Apply record to MemTable
            out_memtable->Add(rec.sequence, rec.type, Slice(rec.key), Slice(rec.value));
            summary.records_applied++;
            summary.latest_replayed_timestamp_micros = rec.timestamp_micros;
        }

        if (cutoff_reached) {
            break;
        }
    }

    if (out_summary) {
        *out_summary = summary;
    }

    return Status::OK();
}

} // namespace lsm
