#include "wal_archiver.hpp"
#include "wal_format.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <iomanip>

namespace fs = std::filesystem;

namespace lsm {

namespace {

// Standard SHA-256 implementation
inline uint32_t Rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t Sigma0(uint32_t x) {
    return Rotr32(x, 2) ^ Rotr32(x, 13) ^ Rotr32(x, 22);
}

inline uint32_t Sigma1(uint32_t x) {
    return Rotr32(x, 6) ^ Rotr32(x, 11) ^ Rotr32(x, 25);
}

inline uint32_t Gamma0(uint32_t x) {
    return Rotr32(x, 7) ^ Rotr32(x, 18) ^ (x >> 3);
}

inline uint32_t Gamma1(uint32_t x) {
    return Rotr32(x, 17) ^ Rotr32(x, 19) ^ (x >> 10);
}

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

void SHA256Transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = Gamma1(w[i - 2]) + w[i - 7] + Gamma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + Sigma1(e) + Ch(e, f, g) + K[i] + w[i];
        uint32_t t2 = Sigma0(a) + Maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

} // namespace

std::string WALArchiver::ComputeSHA256Hex(const uint8_t* data, size_t length) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    uint64_t total_bits = length * 8;
    size_t offset = 0;

    while (length - offset >= 64) {
        SHA256Transform(state, data + offset);
        offset += 64;
    }

    uint8_t tail_block[64];
    size_t rem = length - offset;
    std::memcpy(tail_block, data + offset, rem);
    tail_block[rem] = 0x80;
    rem++;

    if (rem > 56) {
        std::memset(tail_block + rem, 0, 64 - rem);
        SHA256Transform(state, tail_block);
        std::memset(tail_block, 0, 56);
    } else {
        std::memset(tail_block + rem, 0, 56 - rem);
    }

    for (int i = 0; i < 8; ++i) {
        tail_block[56 + i] = static_cast<uint8_t>((total_bits >> ((7 - i) * 8)) & 0xff);
    }
    SHA256Transform(state, tail_block);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 8; ++i) {
        ss << std::setw(8) << state[i];
    }
    return ss.str();
}

std::string ArchivalManifest::ToJSON() const {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"segment_id\": " << segment_id << ",\n"
       << "  \"min_lsn\": " << min_lsn << ",\n"
       << "  \"max_lsn\": " << max_lsn << ",\n"
       << "  \"uncompressed_size\": " << uncompressed_size << ",\n"
       << "  \"compressed_size\": " << compressed_size << ",\n"
       << "  \"checksum_crc32c\": " << checksum_crc32c << ",\n"
       << "  \"sha256_digest\": \"" << sha256_digest << "\",\n"
       << "  \"archive_filename\": \"" << archive_filename << "\",\n"
       << "  \"timestamp_utc\": " << timestamp_utc << "\n"
       << "}\n";
    return ss.str();
}

Status ArchivalManifest::FromJSON(const std::string& json_str, ArchivalManifest* out_manifest) {
    if (!out_manifest) return Status::InvalidArgument("Null output manifest pointer");

    auto extract_uint64 = [&](const std::string& key) -> uint64_t {
        size_t pos = json_str.find("\"" + key + "\":");
        if (pos == std::string::npos) return 0;
        pos = json_str.find_first_of("0123456789", pos);
        if (pos == std::string::npos) return 0;
        size_t end = json_str.find_first_not_of("0123456789", pos);
        return std::stoull(json_str.substr(pos, end - pos));
    };

    auto extract_str = [&](const std::string& key) -> std::string {
        size_t pos = json_str.find("\"" + key + "\": \"");
        if (pos == std::string::npos) return "";
        pos += key.size() + 5;
        size_t end = json_str.find("\"", pos);
        if (end == std::string::npos) return "";
        return json_str.substr(pos, end - pos);
    };

    out_manifest->segment_id = extract_uint64("segment_id");
    out_manifest->min_lsn = extract_uint64("min_lsn");
    out_manifest->max_lsn = extract_uint64("max_lsn");
    out_manifest->uncompressed_size = extract_uint64("uncompressed_size");
    out_manifest->compressed_size = extract_uint64("compressed_size");
    out_manifest->checksum_crc32c = static_cast<uint32_t>(extract_uint64("checksum_crc32c"));
    out_manifest->sha256_digest = extract_str("sha256_digest");
    out_manifest->archive_filename = extract_str("archive_filename");
    out_manifest->timestamp_utc = extract_uint64("timestamp_utc");

    return Status::OK();
}

void WALArchiver::CompressBuffer(const uint8_t* in, size_t in_len, std::string* out) {
    out->clear();
    out->reserve(in_len);

    size_t i = 0;
    while (i < in_len) {
        uint8_t byte = in[i];
        size_t run_len = 1;
        while (i + run_len < in_len && in[i + run_len] == byte && run_len < 255) {
            run_len++;
        }

        if (run_len >= 4 || byte == 0xAA) {
            out->push_back(static_cast<char>(0xAA));
            out->push_back(static_cast<char>(run_len));
            out->push_back(static_cast<char>(byte));
            i += run_len;
        } else {
            out->push_back(static_cast<char>(byte));
            i++;
        }
    }
}

bool WALArchiver::DecompressBuffer(const uint8_t* in, size_t in_len, size_t orig_len, std::string* out) {
    out->clear();
    out->reserve(orig_len);

    size_t i = 0;
    while (i < in_len) {
        uint8_t byte = in[i];
        if (byte == 0xAA) {
            if (i + 2 >= in_len) return false;
            uint8_t count = in[i + 1];
            uint8_t val = in[i + 2];
            out->append(count, static_cast<char>(val));
            i += 3;
        } else {
            out->push_back(static_cast<char>(byte));
            i++;
        }
    }

    return (out->size() == orig_len);
}

WALArchiver::WALArchiver(std::string archive_directory)
    : archive_dir_(std::move(archive_directory)) {
    fs::create_directories(archive_dir_);
    LoadExistingManifests();
}

void WALArchiver::LoadExistingManifests() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(archive_dir_, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            std::ifstream ifs(entry.path().string());
            if (ifs.is_open()) {
                std::stringstream buffer;
                buffer << ifs.rdbuf();
                ArchivalManifest manifest;
                if (ArchivalManifest::FromJSON(buffer.str(), &manifest).ok()) {
                    manifests_[manifest.segment_id] = manifest;
                }
            }
        }
    }
}

Status WALArchiver::ArchiveSegment(
    const std::string& raw_wal_path,
    uint64_t segment_id,
    uint64_t min_lsn,
    uint64_t max_lsn,
    ArchivalManifest* out_manifest
) {
    std::lock_guard<std::mutex> lock(mu_);

    std::ifstream ifs(raw_wal_path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return Status::IOError("Failed to open source WAL file: " + raw_wal_path);
    }

    std::streamsize file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(file_size);
    if (file_size > 0) {
        if (!ifs.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
            return Status::IOError("Failed to read WAL file data: " + raw_wal_path);
        }
    }
    ifs.close();

    // Compute checksums
    uint32_t crc = (file_size > 0) ? wal::CRC32C::Compute(reinterpret_cast<const char*>(buffer.data()), file_size) : 0;
    std::string sha256 = (file_size > 0) ? ComputeSHA256Hex(buffer.data(), file_size) : ComputeSHA256Hex(nullptr, 0);

    // Compress
    std::string compressed;
    if (file_size > 0) {
        CompressBuffer(buffer.data(), file_size, &compressed);
    }

    // Write compressed segment
    std::string arc_filename = "wal_" + std::to_string(segment_id) + ".arc";
    std::string arc_filepath = (fs::path(archive_dir_) / arc_filename).string();

    std::ofstream ofs(arc_filepath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return Status::IOError("Failed to open archive destination: " + arc_filepath);
    }
    if (!compressed.empty()) {
        ofs.write(compressed.data(), compressed.size());
    }
    ofs.flush();
    ofs.close();

    uint64_t now_utc = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    ArchivalManifest manifest;
    manifest.segment_id = segment_id;
    manifest.min_lsn = min_lsn;
    manifest.max_lsn = max_lsn;
    manifest.uncompressed_size = static_cast<uint64_t>(file_size);
    manifest.compressed_size = static_cast<uint64_t>(compressed.size());
    manifest.checksum_crc32c = crc;
    manifest.sha256_digest = sha256;
    manifest.archive_filename = arc_filename;
    manifest.timestamp_utc = now_utc;

    // Write JSON manifest
    std::string json_filename = "manifest_" + std::to_string(segment_id) + ".json";
    std::string json_filepath = (fs::path(archive_dir_) / json_filename).string();

    std::ofstream jfs(json_filepath, std::ios::trunc);
    if (!jfs.is_open()) {
        return Status::IOError("Failed to write manifest: " + json_filepath);
    }
    jfs << manifest.ToJSON();
    jfs.flush();
    jfs.close();

    manifests_[segment_id] = manifest;

    if (out_manifest) {
        *out_manifest = manifest;
    }

    return Status::OK();
}

Status WALArchiver::RestoreSegment(
    uint64_t segment_id,
    const std::string& destination_wal_path
) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = manifests_.find(segment_id);
    if (it == manifests_.end()) {
        return Status::NotFound("Archive manifest not found for segment " + std::to_string(segment_id));
    }

    const ArchivalManifest& manifest = it->second;
    std::string arc_filepath = (fs::path(archive_dir_) / manifest.archive_filename).string();

    std::ifstream ifs(arc_filepath, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return Status::IOError("Failed to open archive file: " + arc_filepath);
    }

    std::streamsize arc_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<uint8_t> compressed(arc_size);
    if (arc_size > 0) {
        if (!ifs.read(reinterpret_cast<char*>(compressed.data()), arc_size)) {
            return Status::IOError("Failed to read archive file: " + arc_filepath);
        }
    }
    ifs.close();

    std::string decompressed;
    if (manifest.uncompressed_size > 0) {
        if (!DecompressBuffer(compressed.data(), arc_size, manifest.uncompressed_size, &decompressed)) {
            return Status::Corruption("Decompression failed for segment " + std::to_string(segment_id));
        }
    }

    // Verify SHA-256 and CRC32C
    if (manifest.uncompressed_size > 0) {
        std::string calc_sha = ComputeSHA256Hex(reinterpret_cast<const uint8_t*>(decompressed.data()), decompressed.size());
        if (calc_sha != manifest.sha256_digest) {
            return Status::Corruption("SHA-256 digest mismatch upon archive restoration");
        }
        uint32_t calc_crc = wal::CRC32C::Compute(decompressed.data(), decompressed.size());
        if (calc_crc != manifest.checksum_crc32c) {
            return Status::Corruption("CRC32C checksum mismatch upon archive restoration");
        }
    }

    std::ofstream ofs(destination_wal_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return Status::IOError("Failed to open destination WAL path: " + destination_wal_path);
    }
    if (!decompressed.empty()) {
        ofs.write(decompressed.data(), decompressed.size());
    }
    ofs.flush();
    ofs.close();

    return Status::OK();
}

Status WALArchiver::VerifyArchive(uint64_t segment_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = manifests_.find(segment_id);
    if (it == manifests_.end()) {
        return Status::NotFound("Archive manifest not found for segment " + std::to_string(segment_id));
    }

    const ArchivalManifest& manifest = it->second;
    std::string arc_filepath = (fs::path(archive_dir_) / manifest.archive_filename).string();

    std::ifstream ifs(arc_filepath, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return Status::IOError("Failed to open archive file: " + arc_filepath);
    }

    std::streamsize arc_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<uint8_t> compressed(arc_size);
    if (arc_size > 0) {
        ifs.read(reinterpret_cast<char*>(compressed.data()), arc_size);
    }
    ifs.close();

    std::string decompressed;
    if (manifest.uncompressed_size > 0) {
        if (!DecompressBuffer(compressed.data(), arc_size, manifest.uncompressed_size, &decompressed)) {
            return Status::Corruption("Decompression failed during archive verification");
        }
        std::string calc_sha = ComputeSHA256Hex(reinterpret_cast<const uint8_t*>(decompressed.data()), decompressed.size());
        if (calc_sha != manifest.sha256_digest) {
            return Status::Corruption("SHA-256 digest verification failed");
        }
    }

    return Status::OK();
}

std::vector<ArchivalManifest> WALArchiver::ListArchives() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ArchivalManifest> result;
    result.reserve(manifests_.size());
    for (const auto& [id, m] : manifests_) {
        result.push_back(m);
    }
    return result;
}

} // namespace lsm
