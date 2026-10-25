#include "wal_truncator.hpp"
#include "wal_reader.hpp"
#include "write_batch.hpp"
#include <filesystem>
#include <system_error>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace lsm {

WALTruncator::WALTruncator(std::string wal_directory)
    : wal_dir_(std::move(wal_directory)) {}

void WALTruncator::RegisterSegment(const WALMetadata& meta) {
    std::lock_guard<std::mutex> lock(mu_);
    segments_[meta.log_number] = meta;
}

void WALTruncator::UpdateSegmentLSN(uint64_t log_num, SequenceNumber min_lsn, SequenceNumber max_lsn) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = segments_.find(log_num);
    if (it != segments_.end()) {
        if (it->second.min_lsn == 0 || min_lsn < it->second.min_lsn) {
            it->second.min_lsn = min_lsn;
        }
        if (max_lsn > it->second.max_lsn) {
            it->second.max_lsn = max_lsn;
        }
    }
}

void WALTruncator::SetSegmentImmutable(uint64_t log_num, bool immutable) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = segments_.find(log_num);
    if (it != segments_.end()) {
        it->second.is_immutable = immutable;
    }
}

Status WALTruncator::ScanDirectory(const std::string& wal_dir) {
    std::lock_guard<std::mutex> lock(mu_);
    wal_dir_ = wal_dir;

    std::error_code ec;
    if (!fs::exists(wal_dir_, ec)) {
        return Status::IOError("Directory does not exist: " + wal_dir_);
    }

    for (const auto& entry : fs::directory_iterator(wal_dir_, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".log") {
            std::string filename = entry.path().filename().string();
            try {
                size_t dot = filename.find('.');
                uint64_t log_num = std::stoull(filename.substr(0, dot));
                uint64_t file_sz = entry.file_size();

                SequenceNumber seg_min = 0;
                SequenceNumber seg_max = 0;

                int fd = ::open(entry.path().string().c_str(), O_RDONLY);
                if (fd >= 0) {
                    auto file = std::make_unique<PosixSequentialFile>(entry.path().string(), fd);
                    WALReader reader(std::move(file));
                    Slice record;
                    std::string scratch;
                    while (reader.ReadRecord(&record, &scratch)) {
                        if (record.size() >= 12) {
                            SequenceNumber seq = WriteBatchInternal::Sequence(&record);
                            uint32_t count = WriteBatchInternal::Count(&record);
                            if (count > 0) {
                                if (seg_min == 0 || seq < seg_min) {
                                    seg_min = seq;
                                }
                                SequenceNumber batch_max = seq + count - 1;
                                if (batch_max > seg_max) {
                                    seg_max = batch_max;
                                }
                            }
                        }
                    }
                }

                WALMetadata meta;
                meta.log_number = log_num;
                meta.path = entry.path().string();
                meta.min_lsn = seg_min;
                meta.max_lsn = seg_max;
                meta.file_size = file_sz;
                meta.is_immutable = true;

                segments_[log_num] = meta;
            } catch (...) {
                // Ignore non-numeric log files
            }
        }
    }

    return Status::OK();
}

std::vector<WALMetadata> WALTruncator::GetEligibleSegments(SequenceNumber flushed_lsn) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<WALMetadata> eligible;
    for (const auto& [seg_id, meta] : segments_) {
        // Obsolete if segment has max_lsn <= flushed_lsn and is marked immutable
        if (meta.is_immutable && meta.max_lsn > 0 && meta.max_lsn <= flushed_lsn) {
            eligible.push_back(meta);
        }
    }
    return eligible;
}

Status WALTruncator::PruneWALs(SequenceNumber flushed_lsn, PruneReport* out_report) {
    std::lock_guard<std::mutex> lock(mu_);
    PruneReport report;
    report.flushed_lsn = flushed_lsn;
    report.files_examined = segments_.size();

    std::vector<uint64_t> to_delete_ids;

    for (auto& [seg_id, meta] : segments_) {
        if (meta.is_immutable && meta.max_lsn > 0 && meta.max_lsn <= flushed_lsn) {
            std::error_code ec;
            if (fs::exists(meta.path, ec)) {
                uint64_t sz = meta.file_size;
                if (fs::remove(meta.path, ec)) {
                    report.files_pruned++;
                    report.bytes_reclaimed += sz;
                    report.pruned_file_paths.push_back(meta.path);
                    to_delete_ids.push_back(seg_id);
                }
            } else {
                to_delete_ids.push_back(seg_id);
            }
        }
    }

    for (uint64_t id : to_delete_ids) {
        segments_.erase(id);
    }

    if (out_report) {
        *out_report = std::move(report);
    }

    return Status::OK();
}

std::vector<WALMetadata> WALTruncator::GetAllSegments() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<WALMetadata> res;
    res.reserve(segments_.size());
    for (const auto& [id, meta] : segments_) {
        res.push_back(meta);
    }
    return res;
}

SequenceNumber WALTruncator::GetMinActiveLSN() const {
    std::lock_guard<std::mutex> lock(mu_);
    SequenceNumber min_active = UINT64_MAX;
    for (const auto& [id, meta] : segments_) {
        if (meta.min_lsn > 0 && meta.min_lsn < min_active) {
            min_active = meta.min_lsn;
        }
    }
    return (min_active == UINT64_MAX) ? 0 : min_active;
}

} // namespace lsm
