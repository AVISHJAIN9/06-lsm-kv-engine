#include "backup_engine.hpp"
#include <fstream>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <unistd.h>
#include <sys/stat.h>

namespace lsm {

namespace fs = std::filesystem;

static bool CopyFileSafe(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in.is_open()) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << in.rdbuf();
    return out.good();
}

BackupEngine::BackupEngine(std::string backup_root_dir)
    : backup_root_dir_(std::move(backup_root_dir)) {
    shared_sst_dir_ = (fs::path(backup_root_dir_) / "shared_sst").string();
    meta_dir_ = (fs::path(backup_root_dir_) / "meta").string();
    InitializeDirectories();
    LoadManifest();
}

void BackupEngine::InitializeDirectories() {
    std::error_code ec;
    fs::create_directories(backup_root_dir_, ec);
    fs::create_directories(shared_sst_dir_, ec);
    fs::create_directories(meta_dir_, ec);
}

Status BackupEngine::CreateNewBackup(
    const std::string& db_dir,
    const std::vector<std::string>& live_sst_files,
    const std::string& manifest_file,
    bool incremental,
    BackupInfo* out_info
) {
    std::lock_guard<std::mutex> lock(mutex_);

    BackupInfo info;
    info.backup_id = next_backup_id_++;
    info.timestamp_micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    info.is_incremental = incremental && !backups_.empty();
    info.manifest_file = manifest_file;
    info.total_size_bytes = 0;

    for (const auto& sst_name : live_sst_files) {
        std::string src_path = (fs::path(db_dir) / sst_name).string();
        std::string shared_dst = (fs::path(shared_sst_dir_) / sst_name).string();

        info.referenced_ssts.push_back(sst_name);

        bool already_exists = existing_shared_ssts_.find(sst_name) != existing_shared_ssts_.end() &&
                              fs::exists(shared_dst);

        if (!already_exists) {
            if (fs::exists(src_path)) {
                if (!CopyFileSafe(src_path, shared_dst)) {
                    return Status::IOError("Failed to copy new SSTable to shared backup storage: " + sst_name);
                }
                existing_shared_ssts_.insert(sst_name);
                info.new_ssts.push_back(sst_name);
            }
        }

        struct stat st;
        if (::stat(shared_dst.c_str(), &st) == 0) {
            info.total_size_bytes += st.st_size;
        }
    }

    // Backup the manifest file to backup-specific meta directory
    if (!manifest_file.empty()) {
        std::string src_manifest = (fs::path(db_dir) / manifest_file).string();
        std::string backup_manifest_dst = (fs::path(meta_dir_) / ("MANIFEST_" + std::to_string(info.backup_id))).string();
        if (fs::exists(src_manifest)) {
            CopyFileSafe(src_manifest, backup_manifest_dst);
        }
    }

    info.number_of_files = info.referenced_ssts.size();
    backups_.push_back(info);

    SaveManifest();

    if (out_info) {
        *out_info = info;
    }

    return Status::OK();
}

Status BackupEngine::RestoreBackup(uint32_t backup_id, const std::string& restore_dir) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(backups_.begin(), backups_.end(), [backup_id](const BackupInfo& b) {
        return b.backup_id == backup_id;
    });

    if (it == backups_.end()) {
        return Status::NotFound("Backup ID not found: " + std::to_string(backup_id));
    }

    std::error_code ec;
    fs::create_directories(restore_dir, ec);

    // Restore referenced SSTables from shared directory
    for (const auto& sst_name : it->referenced_ssts) {
        std::string src_shared = (fs::path(shared_sst_dir_) / sst_name).string();
        std::string dst_restore = (fs::path(restore_dir) / sst_name).string();

        if (!CopyFileSafe(src_shared, dst_restore)) {
            return Status::IOError("Failed to restore SSTable: " + sst_name);
        }
    }

    // Restore MANIFEST and CURRENT file
    if (!it->manifest_file.empty()) {
        std::string backup_manifest_src = (fs::path(meta_dir_) / ("MANIFEST_" + std::to_string(backup_id))).string();
        std::string dst_manifest = (fs::path(restore_dir) / it->manifest_file).string();

        if (fs::exists(backup_manifest_src)) {
            CopyFileSafe(backup_manifest_src, dst_manifest);
        }

        std::string current_dst = (fs::path(restore_dir) / "CURRENT").string();
        std::ofstream cur_out(current_dst, std::ios::trunc);
        if (cur_out.is_open()) {
            cur_out << it->manifest_file << "\n";
        }
    }

    return Status::OK();
}

Status BackupEngine::PurgeOldBackups(uint32_t keep_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backups_.size() <= keep_count) {
        return Status::OK();
    }

    size_t remove_count = backups_.size() - keep_count;
    backups_.erase(backups_.begin(), backups_.begin() + remove_count);

    // Garbage-collect orphaned SSTables in shared_sst
    std::unordered_set<std::string> active_referenced_ssts;
    for (const auto& b : backups_) {
        for (const auto& sst : b.referenced_ssts) {
            active_referenced_ssts.insert(sst);
        }
    }

    for (auto it = existing_shared_ssts_.begin(); it != existing_shared_ssts_.end();) {
        if (active_referenced_ssts.find(*it) == active_referenced_ssts.end()) {
            std::string path_to_remove = (fs::path(shared_sst_dir_) / *it).string();
            ::unlink(path_to_remove.c_str());
            it = existing_shared_ssts_.erase(it);
        } else {
            ++it;
        }
    }

    SaveManifest();
    return Status::OK();
}

Status BackupEngine::GetBackupInfo(uint32_t backup_id, BackupInfo* out_info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(backups_.begin(), backups_.end(), [backup_id](const BackupInfo& b) {
        return b.backup_id == backup_id;
    });

    if (it == backups_.end()) {
        return Status::NotFound("Backup ID not found: " + std::to_string(backup_id));
    }
    if (out_info) *out_info = *it;
    return Status::OK();
}

std::vector<BackupInfo> BackupEngine::ListBackups() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backups_;
}

Status BackupEngine::SaveManifest() {
    std::string path = (fs::path(meta_dir_) / "BACKUP_MANIFEST.meta").string();
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return Status::IOError("Failed to save backup manifest file");
    }

    out << "NEXT_ID=" << next_backup_id_ << "\n";
    for (const auto& b : backups_) {
        out << "BACKUP:" << b.backup_id
            << ":" << b.timestamp_micros
            << ":" << b.total_size_bytes
            << ":" << b.number_of_files
            << ":" << (b.is_incremental ? "1" : "0")
            << ":" << b.manifest_file << "\n";
        out << "FILES:";
        for (size_t i = 0; i < b.referenced_ssts.size(); ++i) {
            out << b.referenced_ssts[i] << (i + 1 < b.referenced_ssts.size() ? "," : "");
        }
        out << "\n";
    }
    return Status::OK();
}

Status BackupEngine::LoadManifest() {
    std::string path = (fs::path(meta_dir_) / "BACKUP_MANIFEST.meta").string();
    std::ifstream in(path);
    if (!in.is_open()) {
        return Status::OK(); // New empty engine
    }

    backups_.clear();
    existing_shared_ssts_.clear();

    std::string line;
    BackupInfo cur_backup;
    bool has_cur = false;

    while (std::getline(in, line)) {
        if (line.rfind("NEXT_ID=", 0) == 0) {
            next_backup_id_ = std::stoul(line.substr(8));
        } else if (line.rfind("BACKUP:", 0) == 0) {
            if (has_cur) {
                backups_.push_back(cur_backup);
            }
            cur_backup = BackupInfo();
            has_cur = true;

            std::stringstream ss(line.substr(7));
            std::string token;
            if (std::getline(ss, token, ':')) cur_backup.backup_id = std::stoul(token);
            if (std::getline(ss, token, ':')) cur_backup.timestamp_micros = std::stoll(token);
            if (std::getline(ss, token, ':')) cur_backup.total_size_bytes = std::stoull(token);
            if (std::getline(ss, token, ':')) cur_backup.number_of_files = std::stoull(token);
            if (std::getline(ss, token, ':')) cur_backup.is_incremental = (token == "1");
            if (std::getline(ss, token, ':')) cur_backup.manifest_file = token;
        } else if (line.rfind("FILES:", 0) == 0) {
            std::string files_str = line.substr(6);
            std::stringstream ss(files_str);
            std::string sst;
            while (std::getline(ss, sst, ',')) {
                if (!sst.empty()) {
                    cur_backup.referenced_ssts.push_back(sst);
                    existing_shared_ssts_.insert(sst);
                }
            }
        }
    }

    if (has_cur) {
        backups_.push_back(cur_backup);
    }

    return Status::OK();
}

} // namespace lsm
