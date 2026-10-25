#include "checkpoint_manager.hpp"
#include <fstream>
#include <chrono>
#include <filesystem>
#include <unistd.h>
#include <sys/stat.h>

namespace lsm {

namespace fs = std::filesystem;

static bool CopyFileFallback(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in.is_open()) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << in.rdbuf();
    return out.good();
}

Status LiveCheckpointManager::CreateCheckpoint(
    const std::string& db_dir,
    const std::string& checkpoint_dir,
    const std::vector<std::string>& live_sst_files,
    const std::string& manifest_filename,
    const std::vector<std::string>& wal_files,
    uint64_t current_seq,
    CheckpointMetadata* out_meta
) {
    std::error_code ec;
    fs::create_directories(checkpoint_dir, ec);
    if (ec) {
        return Status::IOError("Failed to create checkpoint directory: " + ec.message());
    }

    CheckpointMetadata meta;
    meta.checkpoint_id = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    meta.sequence_number = current_seq;
    meta.timestamp_micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    meta.checkpoint_dir = checkpoint_dir;
    meta.manifest_file = manifest_filename;
    meta.used_hard_links = true;
    meta.total_bytes = 0;

    // 1. Hard-link all immutable live SSTables
    for (const auto& sst_name : live_sst_files) {
        std::string src_path = (fs::path(db_dir) / sst_name).string();
        std::string dst_path = (fs::path(checkpoint_dir) / sst_name).string();

        if (fs::exists(src_path)) {
            // Unlink destination if already present
            ::unlink(dst_path.c_str());

            int res = ::link(src_path.c_str(), dst_path.c_str());
            if (res != 0) {
                // Fallback to copy if cross-filesystem or link not supported
                if (!CopyFileFallback(src_path, dst_path)) {
                    return Status::IOError("Failed to hardlink or copy SSTable: " + sst_name);
                }
                meta.used_hard_links = false;
            }

            struct stat st;
            if (::stat(dst_path.c_str(), &st) == 0) {
                meta.total_bytes += st.st_size;
            }
            meta.sstable_files.push_back(sst_name);
        }
    }

    // 2. Copy active MANIFEST file
    if (!manifest_filename.empty()) {
        std::string src_manifest = (fs::path(db_dir) / manifest_filename).string();
        std::string dst_manifest = (fs::path(checkpoint_dir) / manifest_filename).string();
        if (fs::exists(src_manifest)) {
            if (!CopyFileFallback(src_manifest, dst_manifest)) {
                return Status::IOError("Failed to copy MANIFEST file to checkpoint");
            }
            struct stat st;
            if (::stat(dst_manifest.c_str(), &st) == 0) {
                meta.total_bytes += st.st_size;
            }
        }
    }

    // 3. Write CURRENT pointer file
    std::string current_path = (fs::path(checkpoint_dir) / "CURRENT").string();
    std::ofstream current_out(current_path, std::ios::trunc);
    if (current_out.is_open()) {
        current_out << manifest_filename << "\n";
        current_out.close();
    }

    // 4. Hard-link or copy WAL files
    for (const auto& wal_name : wal_files) {
        std::string src_wal = (fs::path(db_dir) / wal_name).string();
        std::string dst_wal = (fs::path(checkpoint_dir) / wal_name).string();
        if (fs::exists(src_wal)) {
            ::unlink(dst_wal.c_str());
            if (::link(src_wal.c_str(), dst_wal.c_str()) != 0) {
                CopyFileFallback(src_wal, dst_wal);
            }
            meta.wal_files.push_back(wal_name);
        }
    }

    if (out_meta) {
        *out_meta = meta;
    }

    return Status::OK();
}

bool LiveCheckpointManager::VerifyHardLinks(const std::string& src_file, const std::string& dst_file) {
    struct stat st_src;
    struct stat st_dst;

    if (::stat(src_file.c_str(), &st_src) != 0) return false;
    if (::stat(dst_file.c_str(), &st_dst) != 0) return false;

    return (st_src.st_ino == st_dst.st_ino) && (st_src.st_dev == st_dst.st_dev);
}

} // namespace lsm
