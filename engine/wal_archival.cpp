#include "wal_archival.hpp"
#include "wal_format.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstring>

namespace lsm {

namespace fs = std::filesystem;

std::string ArchiveManifest::ToJson() const {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"log_number\": " << log_number << ",\n"
        << "  \"original_bytes\": " << original_bytes << ",\n"
        << "  \"compressed_bytes\": " << compressed_bytes << ",\n"
        << "  \"crc32c\": " << crc32c << ",\n"
        << "  \"min_lsn\": " << min_lsn << ",\n"
        << "  \"max_lsn\": " << max_lsn << ",\n"
        << "  \"timestamp\": " << archive_timestamp << ",\n"
        << "  \"archive_file\": \"" << archive_file << "\",\n"
        << "  \"algorithm\": \"" << algorithm << "\"\n"
        << "}";
    return oss.str();
}

ColdTierArchiver::ColdTierArchiver(std::string archive_dir)
    : archive_dir_(std::move(archive_dir)) {
    std::error_code ec;
    fs::create_directories(archive_dir_, ec);
}

// Fast byte-level LZ/RLE compression format:
// Header: [Magic: "ZPK1" (4B)] [OrigLen: uint32 (4B)] [CRC32C: uint32 (4B)] [Data...]
std::string ColdTierArchiver::Compress(const std::string& input) {
    uint32_t orig_len = static_cast<uint32_t>(input.size());
    uint32_t crc = wal::CRC32C::Value(input.data(), input.size());

    std::string out;
    out.reserve(12 + input.size());
    out.append("ZPK1", 4);

    // 4B length
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((orig_len >> (i * 8)) & 0xff));
    // 4B CRC32C
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((crc >> (i * 8)) & 0xff));

    // Byte-level LZ match / run compression
    const uint8_t* src = reinterpret_cast<const uint8_t*>(input.data());
    size_t i = 0;
    while (i < input.size()) {
        uint8_t b = src[i];
        size_t run = 1;
        while (i + run < input.size() && src[i + run] == b && run < 255) {
            run++;
        }

        if (run >= 4) {
            // Marker 0xFF, byte, count
            out.push_back(static_cast<char>(0xff));
            out.push_back(static_cast<char>(b));
            out.push_back(static_cast<char>(run));
            i += run;
        } else {
            if (b == 0xff) {
                out.push_back(static_cast<char>(0xff));
                out.push_back(static_cast<char>(0xff));
                out.push_back(static_cast<char>(1));
            } else {
                out.push_back(static_cast<char>(b));
            }
            i++;
        }
    }

    return out;
}

Status ColdTierArchiver::Decompress(const std::string& input, std::string* output) {
    if (!output) return Status::InvalidArgument("Null output string pointer");
    if (input.size() < 12) return Status::Corruption("Archive payload too short");
    if (std::memcmp(input.data(), "ZPK1", 4) != 0) {
        return Status::Corruption("Invalid archive magic header");
    }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(input.data() + 4);
    uint32_t expected_len = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    uint32_t expected_crc = p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24);

    output->clear();
    output->reserve(expected_len);

    size_t i = 12;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(input.data());
    while (i < input.size()) {
        uint8_t b = src[i++];
        if (b == 0xff) {
            if (i + 1 >= input.size()) return Status::Corruption("Truncated compression marker");
            uint8_t sym = src[i++];
            uint8_t count = src[i++];
            output->append(count, static_cast<char>(sym));
        } else {
            output->push_back(static_cast<char>(b));
        }
    }

    if (output->size() != expected_len) {
        return Status::Corruption("Decompressed size does not match header");
    }

    uint32_t actual_crc = wal::CRC32C::Value(output->data(), output->size());
    if (actual_crc != expected_crc) {
        return Status::Corruption("Decompressed CRC32C checksum mismatch");
    }

    return Status::OK();
}

Status ColdTierArchiver::ArchiveSegment(
    uint64_t log_num,
    const std::string& source_wal_path,
    SequenceNumber min_lsn,
    SequenceNumber max_lsn,
    ArchiveManifest* out_manifest
) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Read source WAL file
    std::ifstream ifs(source_wal_path, std::ios::binary);
    if (!ifs) {
        return Status::IOError("Failed to open source WAL file for archival: " + source_wal_path);
    }
    std::string raw_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    std::string compressed = Compress(raw_content);

    std::ostringstream archive_name;
    archive_name << "wal_" << std::setfill('0') << std::setw(6) << log_num << ".zst";
    std::string dest_path = archive_dir_ + "/" + archive_name.str();

    std::ofstream ofs(dest_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Status::IOError("Failed to open archive destination: " + dest_path);
    }
    ofs.write(compressed.data(), compressed.size());
    ofs.close();

    ArchiveManifest manifest;
    manifest.log_number = log_num;
    manifest.original_bytes = raw_content.size();
    manifest.compressed_bytes = compressed.size();
    manifest.crc32c = wal::CRC32C::Value(raw_content.data(), raw_content.size());
    manifest.min_lsn = min_lsn;
    manifest.max_lsn = max_lsn;
    manifest.archive_timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    manifest.archive_file = dest_path;

    // Write manifest file
    std::string manifest_path = archive_dir_ + "/wal_" + std::to_string(log_num) + ".manifest.json";
    std::ofstream mfs(manifest_path, std::ios::trunc);
    if (mfs) {
        mfs << manifest.ToJson();
        mfs.close();
    }

    manifests_.push_back(manifest);
    if (out_manifest) {
        *out_manifest = manifest;
    }

    return Status::OK();
}

Status ColdTierArchiver::RestoreSegment(
    const std::string& archive_path,
    const std::string& restored_wal_path,
    uint32_t expected_crc
) {
    std::ifstream ifs(archive_path, std::ios::binary);
    if (!ifs) {
        return Status::IOError("Failed to open archive for restore: " + archive_path);
    }
    std::string compressed((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    std::string decompressed;
    Status s = Decompress(compressed, &decompressed);
    if (!s.ok()) return s;

    if (expected_crc != 0) {
        uint32_t actual = wal::CRC32C::Value(decompressed.data(), decompressed.size());
        if (actual != expected_crc) {
            return Status::Corruption("Restored WAL checksum failed expected CRC verification");
        }
    }

    std::ofstream ofs(restored_wal_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Status::IOError("Failed to open restore target: " + restored_wal_path);
    }
    ofs.write(decompressed.data(), decompressed.size());
    ofs.close();

    return Status::OK();
}

std::vector<ArchiveManifest> ColdTierArchiver::ListArchives() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return manifests_;
}

} // namespace lsm
