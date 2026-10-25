#include "multi_cf_recovery.hpp"
#include "current_file.hpp"
#include "wal_format.hpp"
#include <fstream>
#include <vector>

namespace lsm {

namespace {

bool DecodeVarint32(Slice* input, uint32_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    uint32_t result = 0;
    for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
        uint32_t byte = static_cast<uint8_t>(*p++);
        if (byte & 128) {
            result |= ((byte & 127) << shift);
        } else {
            result |= (byte << shift);
            *input = Slice(p, limit - p);
            *value = result;
            return true;
        }
    }
    return false;
}

bool DecodeLengthPrefixedSlice(Slice* input, Slice* result) {
    uint32_t len = 0;
    if (DecodeVarint32(input, &len) && input->size() >= len) {
        *result = Slice(input->data(), len);
        input->remove_prefix(len);
        return true;
    }
    return false;
}

} // anonymous namespace

Status MultiCFManifestRecovery::Recover(
    const std::string& dbname,
    ColumnFamilySet* cf_set,
    MultiCFVersionSet* vset,
    MultiCFRecoveryStats* stats
) {
    if (!cf_set || !vset) {
        return Status::InvalidArgument("Null pointer passed to ManifestRecovery");
    }

    std::string manifest_filename;
    Status s = ReadCurrentFile(dbname, &manifest_filename);
    if (!s.ok()) {
        return s;
    }

    std::string manifest_path = dbname + "/" + manifest_filename;
    return ReplayManifestFile(manifest_path, cf_set, vset, stats);
}

Status MultiCFManifestRecovery::ReplayManifestFile(
    const std::string& manifest_path,
    ColumnFamilySet* cf_set,
    MultiCFVersionSet* vset,
    MultiCFRecoveryStats* stats
) {
    std::ifstream file(manifest_path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return Status::IOError("Failed to open MANIFEST file: " + manifest_path);
    }

    MultiCFRecoveryStats local_stats;

    char header[8];
    while (file.read(header, 8)) {
        uint32_t length = static_cast<uint8_t>(header[0]) |
                          (static_cast<uint8_t>(header[1]) << 8) |
                          (static_cast<uint8_t>(header[2]) << 16) |
                          (static_cast<uint8_t>(header[3]) << 24);

        uint32_t masked_crc = static_cast<uint8_t>(header[4]) |
                              (static_cast<uint8_t>(header[5]) << 8) |
                              (static_cast<uint8_t>(header[6]) << 16) |
                              (static_cast<uint8_t>(header[7]) << 24);

        std::string record_buf(length, '\0');
        if (!file.read(&record_buf[0], length)) {
            return Status::Corruption("Truncated record in MANIFEST file: " + manifest_path);
        }

        uint32_t computed_crc = wal::CRC32C::Value(record_buf.data(), record_buf.size());
        if (wal::CRC32C::Mask(computed_crc) != masked_crc) {
            return Status::Corruption("Checksum mismatch in MANIFEST record: " + manifest_path);
        }

        local_stats.records_replayed++;

        // Slice through the record buffer which contains length-prefixed edits
        Slice record_slice(record_buf);
        Slice edit_slice;
        while (DecodeLengthPrefixedSlice(&record_slice, &edit_slice)) {
            MultiCFVersionEdit edit;
            Status decode_status = edit.DecodeFrom(edit_slice);
            if (!decode_status.ok()) {
                return decode_status;
            }

            local_stats.edits_replayed++;

            // Replay column family additions
            for (const auto& add : edit.added_cfs()) {
                if (!cf_set->GetColumnFamily(add.cf_id)) {
                    cf_set->CreateColumnFamily(add.cf_name);
                }
            }

            // Replay column family drops
            for (const auto& drop : edit.dropped_cfs()) {
                auto handle = cf_set->GetColumnFamily(drop.cf_id);
                if (handle && handle->cfd()) {
                    cf_set->DropColumnFamily(handle.get());
                }
            }

            // Replay new files
            for (const auto& nf : edit.new_files()) {
                auto handle = cf_set->GetColumnFamily(nf.cf_id);
                if (handle && handle->cfd()) {
                    handle->cfd()->AddLevelFile(nf.level, nf.file_meta);
                    local_stats.files_recovered++;
                }
            }

            if (edit.has_last_sequence()) {
                vset->SetLastSequence(edit.last_sequence());
                local_stats.last_sequence = edit.last_sequence();
            }

            if (edit.has_next_file_number()) {
                vset->SetNextFileNumber(edit.next_file_number());
                local_stats.next_file_number = edit.next_file_number();
            }
        }
    }

    local_stats.cfs_recovered = static_cast<uint32_t>(cf_set->Count());

    if (stats) {
        *stats = local_stats;
    }

    return Status::OK();
}

} // namespace lsm
