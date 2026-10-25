#include "manifest_compactor.hpp"
#include "current_file.hpp"
#include "wal_format.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace fs = std::filesystem;

namespace lsm {

Status ManifestCompactor::RollManifest(
    const std::string& dbname,
    VersionSet* vset,
    uint64_t* out_new_manifest_num,
    std::string* out_old_manifest_filename
) {
    if (!vset) {
        return Status::InvalidArgument("Null VersionSet provided to ManifestCompactor");
    }

    std::string old_manifest_filename;
    Status s = ReadCurrentFile(dbname, &old_manifest_filename);
    if (!s.ok()) {
        // Fall back if CURRENT does not exist
        std::ostringstream oss;
        oss << "MANIFEST-" << std::setw(6) << std::setfill('0') << vset->ManifestFileNumber();
        old_manifest_filename = oss.str();
    }

    if (out_old_manifest_filename) {
        *out_old_manifest_filename = old_manifest_filename;
    }

    uint64_t new_manifest_num = vset->NewFileNumber();
    std::ostringstream oss;
    oss << "MANIFEST-" << std::setw(6) << std::setfill('0') << new_manifest_num;
    std::string new_manifest_filename = oss.str();
    std::string new_manifest_path = dbname + "/" + new_manifest_filename;

    std::ofstream file(new_manifest_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return Status::IOError("Failed to create new MANIFEST file: " + new_manifest_path);
    }

    // 1. Build consolidated snapshot VersionEdit
    VersionEdit snapshot_edit;
    snapshot_edit.SetComparatorName(Slice(vset->comparator_name()));
    snapshot_edit.SetNextFile(vset->NextFileNumber());
    snapshot_edit.SetLastSequence(vset->LastSequence());
    snapshot_edit.SetLogNumber(vset->LogNumber());

    Version* cur = vset->current();
    if (cur) {
        for (int l = 0; l < Version::kNumLevels; ++l) {
            for (const auto& f : cur->files(l)) {
                snapshot_edit.AddFile(l, f);
            }
            if (!cur->compact_pointer(l).empty()) {
                InternalKey cp_key;
                cp_key.DecodeFrom(Slice(cur->compact_pointer(l)));
                snapshot_edit.SetCompactPointer(l, cp_key);
            }
        }
    }

    // 2. Encode and write to new manifest file
    std::string record;
    snapshot_edit.EncodeTo(&record);

    uint32_t crc = wal::CRC32C::Value(record.data(), record.size());
    crc = wal::CRC32C::Mask(crc);
    uint16_t length = static_cast<uint16_t>(record.size());
    uint8_t type = 1;

    char header[7];
    for (int i = 0; i < 4; ++i) {
        header[i] = static_cast<char>((crc >> (i * 8)) & 0xFF);
    }
    header[4] = static_cast<char>(length & 0xFF);
    header[5] = static_cast<char>((length >> 8) & 0xFF);
    header[6] = static_cast<char>(type);

    file.write(header, 7);
    file.write(record.data(), record.size());
    file.flush();
    file.close();

    // 3. Atomically point CURRENT to new manifest file
    s = SetCurrentFile(dbname, new_manifest_num);
    if (!s.ok()) {
        return s;
    }

    vset->SetManifestFileNumber(new_manifest_num);

    // 4. Safe garbage-collection of old manifest
    std::string old_manifest_path = dbname + "/" + old_manifest_filename;
    std::error_code ec;
    fs::remove(old_manifest_path, ec);

    if (out_new_manifest_num) {
        *out_new_manifest_num = new_manifest_num;
    }

    return Status::OK();
}

} // namespace lsm
