#include "manifest_recovery.hpp"
#include "current_file.hpp"
#include "wal_format.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace lsm {

Status ManifestRecovery::RecoverAndVerify(
    const std::string& dbname,
    VersionSet* vset,
    size_t* out_edits_applied,
    size_t* out_files_verified,
    bool verify_sstable_files
) {
    if (!vset) {
        return Status::InvalidArgument("Null VersionSet provided to ManifestRecovery");
    }

    std::string manifest_filename;
    Status s = ReadCurrentFile(dbname, &manifest_filename);
    if (!s.ok()) {
        return s;
    }

    std::string manifest_path = dbname + "/" + manifest_filename;
    std::ifstream file(manifest_path, std::ios::binary);
    if (!file.is_open()) {
        return Status::IOError("Failed to open manifest file: " + manifest_path);
    }

    VersionBuilder builder(vset, vset->current());
    std::string buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Slice input(buffer);

    size_t edits_count = 0;

    while (input.size() >= 7) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(input.data());
        uint32_t expected_crc = 0;
        for (int i = 0; i < 4; ++i) {
            expected_crc |= (static_cast<uint32_t>(p[i]) << (i * 8));
        }
        uint16_t length = static_cast<uint16_t>(p[4]) | (static_cast<uint16_t>(p[5]) << 8);
        input.remove_prefix(7);

        if (input.size() < length) {
            return Status::Corruption("Truncated manifest record in " + manifest_filename);
        }

        Slice record(input.data(), length);
        input.remove_prefix(length);

        uint32_t actual_crc = wal::CRC32C::Value(record.data(), record.size());
        actual_crc = wal::CRC32C::Mask(actual_crc);
        if (actual_crc != expected_crc) {
            return Status::Corruption("Manifest CRC32C mismatch in " + manifest_filename);
        }

        VersionEdit edit;
        s = edit.DecodeFrom(record);
        if (!s.ok()) {
            return s;
        }

        if (edit.has_comparator() && edit.comparator() != vset->comparator_name()) {
            return Status::InvalidArgument("Manifest comparator mismatch: " + edit.comparator());
        }
        if (edit.has_log_number()) vset->SetLogNumber(edit.log_number());
        if (edit.has_next_file_number()) vset->SetNextFileNumber(edit.next_file_number());
        if (edit.has_last_sequence()) vset->SetLastSequence(edit.last_sequence());

        builder.Apply(&edit);
        edits_count++;
    }

    auto* v = new Version(vset);
    builder.SaveTo(v);
    vset->AppendVersion(v);

    size_t files_verified = 0;
    if (verify_sstable_files) {
        for (int l = 0; l < Version::kNumLevels; ++l) {
            for (const auto& f : v->files(l)) {
                char sst_name[64];
                snprintf(sst_name, sizeof(sst_name), "/%06llu.sst", static_cast<unsigned long long>(f.file_number));
                std::string sst_path = dbname + sst_name;
                std::error_code ec;
                if (!fs::exists(sst_path, ec)) {
                    return Status::Corruption("Referenced SSTable file missing on disk: " + sst_path);
                }
                files_verified++;
            }
        }
    } else {
        files_verified = v->TotalFiles();
    }

    if (out_edits_applied) *out_edits_applied = edits_count;
    if (out_files_verified) *out_files_verified = files_verified;

    return Status::OK();
}

} // namespace lsm
