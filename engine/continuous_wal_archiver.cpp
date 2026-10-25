#include "continuous_wal_archiver.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <cstring>
#include <algorithm>

namespace lsm {

namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// Cryptographic SHA-256 Implementation (FIPS 180-4)
// -----------------------------------------------------------------------------
namespace {

struct SHA256Context {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
};

static inline uint32_t RightRotate(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32 - count));
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void SHA256Transform(uint32_t state[8], const uint8_t data[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    uint32_t w[64];

    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
               (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(data[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = RightRotate(w[i - 15], 7) ^ RightRotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = RightRotate(w[i - 2], 17) ^ RightRotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = RightRotate(e, 6) ^ RightRotate(e, 11) ^ RightRotate(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = RightRotate(a, 2) ^ RightRotate(a, 13) ^ RightRotate(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void SHA256Init(SHA256Context* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void SHA256Update(SHA256Context* ctx, const uint8_t* data, size_t len) {
    size_t buffer_index = static_cast<size_t>(ctx->count & 63);
    ctx->count += len;

    if (buffer_index > 0) {
        size_t part_len = 64 - buffer_index;
        if (len >= part_len) {
            std::memcpy(&ctx->buffer[buffer_index], data, part_len);
            SHA256Transform(ctx->state, ctx->buffer);
            data += part_len;
            len -= part_len;
            buffer_index = 0;
        } else {
            std::memcpy(&ctx->buffer[buffer_index], data, len);
            return;
        }
    }

    while (len >= 64) {
        SHA256Transform(ctx->state, data);
        data += 64;
        len -= 64;
    }

    if (len > 0) {
        std::memcpy(ctx->buffer, data, len);
    }
}

static void SHA256Final(SHA256Context* ctx, uint8_t digest[32]) {
    uint8_t final_count[8];
    uint64_t bit_count = ctx->count * 8;
    for (int i = 0; i < 8; ++i) {
        final_count[7 - i] = static_cast<uint8_t>(bit_count >> (i * 8));
    }

    uint8_t pad = 0x80;
    SHA256Update(ctx, &pad, 1);

    uint8_t zero = 0;
    while ((ctx->count & 63) != 56) {
        SHA256Update(ctx, &zero, 1);
    }

    SHA256Update(ctx, final_count, 8);

    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<uint8_t>(ctx->state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(ctx->state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(ctx->state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(ctx->state[i]);
    }
}

// CRC32C Castagnoli
static const uint32_t CRC32C_TABLE[256] = {
    0x00000000, 0xF26B8303, 0xE13B70F7, 0x1350F3F4, 0xC79A971F, 0x35F1141C, 0x26A1E7E8, 0xD4CA64EB,
    0x8AD958CF, 0x78B2DBCC, 0x6BE22838, 0x9989AB3B, 0x4D43CFD0, 0xBF284CD3, 0xAC78BF27, 0x5E133C24,
    0x105EC76F, 0xE235446C, 0xF165B798, 0x030E349B, 0xD7C45070, 0x25AFD373, 0x36FF2087, 0xC494A384,
    0x9A879FA0, 0x68EC1CA3, 0x7BBCEF57, 0x89D76C54, 0x5D1D08BF, 0xAF768BBC, 0xBC267848, 0x4E4DFB4B,
    0x20BD8EDE, 0xD2D60DDD, 0xC186FE29, 0x33ED7D2A, 0xE72719C1, 0x154C9AC2, 0x061C6936, 0xF477EA35,
    0xAA64D611, 0x580F5512, 0x4B5FA6E6, 0xB93425E5, 0x6DFE410E, 0x9F95C20D, 0x8CC531F9, 0x7EAEB2FA,
    0x30E349B1, 0xC288CAB2, 0xD1D83946, 0x23B3BA45, 0xF779DEAE, 0x05125DAD, 0x1642AE59, 0xE4292D5A,
    0xBA3A117E, 0x4851927D, 0x5B016189, 0xA96AE28A, 0x7DA08661, 0x8FCB0562, 0x9C9BF696, 0x6EF07595,
    0x417B1DBC, 0xB3109EBF, 0xA0406D4B, 0x522BEE48, 0x86E18AA3, 0x748A09A0, 0x67DAFA54, 0x95B17957,
    0xCBA24573, 0x39C9C670, 0x2A993584, 0xD8F2B687, 0x0C38D26C, 0xFE53516F, 0xED03A29B, 0x1F682198,
    0x5125DAD3, 0xA34E59D0, 0xB01EAA24, 0x42752927, 0x96BF4DC6, 0x64D4CECE, 0x77843D35, 0x85EFBE36,
    0xDBFC821C, 0x2997011F, 0x3AC7F2EB, 0xC8AC71E8, 0x1C661503, 0xEE0D9600, 0xFD5D65F4, 0x0F36E6F7,
    0x61C69362, 0x93AD1061, 0x80FDE395, 0x72966096, 0xA65C047D, 0x5437877E, 0x4767748A, 0xB50CF789,
    0xEB1FCBAD, 0x197448AE, 0x0A24BB5A, 0xF84F3859, 0x2C855CB2, 0xDEEEDFB1, 0xCDBE2C45, 0x3FD5AF46,
    0x7198540D, 0x83F3D70E, 0x90A324FA, 0x62C8A7F9, 0xB602C312, 0x44694011, 0x5739B3E5, 0xA55230E6,
    0xFB410CC2, 0x092A8FC1, 0x1A7A7C35, 0xE811FF36, 0x3CDB9BDD, 0xCEB018DE, 0xDDE0EB2A, 0x2F8B6829,
    0x82F63B78, 0x709DB87B, 0x63CD4B8F, 0x91A6C88C, 0x456CAC67, 0xB7072F64, 0xA457DC90, 0x563C5F93,
    0x082F63B7, 0xFA44E0B4, 0xE9141340, 0x1B7F9043, 0xCFB5F4A8, 0x3DDE77AB, 0x2E8E845F, 0xDCE5075C,
    0x92A8FC17, 0x60C37F14, 0x73938CE0, 0x81F80FE3, 0x55326B08, 0xA759E80B, 0xB4091BFF, 0x466298FC,
    0x1871A4D8, 0xEA1A27DB, 0xF94AD42F, 0x0B21572C, 0xDFEB33C7, 0x2D80B0C4, 0x3ED04330, 0xCCBBC033,
    0xA24BB5A6, 0x502036A5, 0x4370C551, 0xB11B4652, 0x65D122B9, 0x97BAA1BA, 0x84EA524E, 0x7681D14D,
    0x2892ED69, 0xDAF96E6A, 0xC9A99D9E, 0x3BC21E9D, 0xEF087A76, 0x1D63F975, 0x0E330A81, 0xFC588982,
    0xB21572C9, 0x407EF1CA, 0x532E023E, 0xA145813D, 0x758FE5D6, 0x87E466D5, 0x94B49521, 0x66DF1622,
    0x38CC2A06, 0xCAA7A905, 0xD9F75AF1, 0x2B9CD9F2, 0xFF56BD19, 0x0D3D3E1A, 0x1E6DCDEE, 0xEC064EED,
    0xC38D26C4, 0x31E6A5C7, 0x22B65633, 0xD0DDD530, 0x0417B1DB, 0xF67C32D8, 0xE52CC12C, 0x1747422F,
    0x49547E0B, 0xBB3FFD08, 0xA86F0EFC, 0x5A048DFF, 0x8ECEE914, 0x7CA56A17, 0x6FF599E3, 0x9D9E1AE0,
    0xD3D3E1AB, 0x21B862A8, 0x32E8915C, 0xC083125F, 0x144976B4, 0xE622F5B7, 0xF5720643, 0x07198540,
    0x590AB964, 0xAB613A67, 0xB831C993, 0x4A5A4A90, 0x9E902E7B, 0x6CFBAD78, 0x7FAB5E8C, 0x8DC0DD8F,
    0xE330A81A, 0x115B2B19, 0x020BD8ED, 0xF0605BEE, 0x24AA3F05, 0xD6C1BC06, 0xC5914FF2, 0x37FACCF1,
    0x69E9F0D5, 0x9B8273D6, 0x88D28022, 0x7AB90321, 0xAE7367CA, 0x5C18E4C9, 0x4F48173D, 0xBD23943E,
    0xF36E6F75, 0x0105EC76, 0x12551F82, 0xE03E9C81, 0x34F4F86A, 0xC69F7B69, 0xD5CF889D, 0x27A40B9E,
    0x79B737BA, 0x8BDCB4B9, 0x988C474D, 0x6AE7C44E, 0xBE2DA0A5, 0x4C4623A6, 0x5F16D052, 0xAD7D5351
};

} // namespace

uint32_t ContinuousWALArchiver::ComputeCRC32C(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        crc = CRC32C_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

std::string ContinuousWALArchiver::ComputeSHA256(const uint8_t* data, size_t len) {
    SHA256Context ctx;
    SHA256Init(&ctx);
    SHA256Update(&ctx, data, len);
    uint8_t digest[32];
    SHA256Final(&ctx, digest);

    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return oss.str();
}

ContinuousWALArchiver::ContinuousWALArchiver(std::string archive_dir)
    : archive_dir_(std::move(archive_dir)) {
    InitializeDirectory();
    LoadArchiveIndex();
}

void ContinuousWALArchiver::InitializeDirectory() {
    std::error_code ec;
    fs::create_directories(archive_dir_, ec);
}

Status ContinuousWALArchiver::ArchiveSegment(
    uint64_t segment_id,
    const std::string& raw_wal_path,
    uint64_t min_seq,
    uint64_t max_seq,
    int64_t min_ts,
    int64_t max_ts,
    ArchivedSegmentMeta* out_meta
) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ifstream in(raw_wal_path, std::ios::binary);
    if (!in.is_open()) {
        return Status::IOError("Could not open raw WAL for archiving: " + raw_wal_path);
    }

    std::string raw_data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    uint64_t raw_size = raw_data.size();
    uint32_t crc = ComputeCRC32C(reinterpret_cast<const uint8_t*>(raw_data.data()), raw_size);
    std::string sha = ComputeSHA256(reinterpret_cast<const uint8_t*>(raw_data.data()), raw_size);

    std::string archive_filename = "wal_" + std::to_string(segment_id) + ".arc";
    std::string archive_full_path = (fs::path(archive_dir_) / archive_filename).string();

    std::ofstream out(archive_full_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Status::IOError("Failed to write compressed archive file: " + archive_full_path);
    }

    // Write header: [RAW_SIZE 8B][CRC32C 4B][DATA]
    uint64_t size_le = raw_size;
    uint32_t crc_le = crc;
    out.write(reinterpret_cast<const char*>(&size_le), sizeof(size_le));
    out.write(reinterpret_cast<const char*>(&crc_le), sizeof(crc_le));
    out.write(raw_data.data(), raw_size);
    out.close();

    ArchivedSegmentMeta meta;
    meta.segment_id = segment_id;
    meta.archive_path = archive_filename;
    meta.min_seq = min_seq;
    meta.max_seq = max_seq;
    meta.min_timestamp_micros = min_ts;
    meta.max_timestamp_micros = max_ts;
    meta.raw_size = raw_size;
    meta.compressed_size = raw_size + 12;
    meta.crc32c = crc;
    meta.sha256_hex = sha;

    segments_.push_back(meta);
    SaveArchiveIndex();

    if (out_meta) *out_meta = meta;
    return Status::OK();
}

Status ContinuousWALArchiver::RestoreSegment(uint64_t segment_id, const std::string& dst_wal_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(segments_.begin(), segments_.end(), [segment_id](const ArchivedSegmentMeta& m) {
        return m.segment_id == segment_id;
    });

    if (it == segments_.end()) {
        return Status::NotFound("Archived segment ID not found: " + std::to_string(segment_id));
    }

    std::string archive_full_path = (fs::path(archive_dir_) / it->archive_path).string();
    std::ifstream in(archive_full_path, std::ios::binary);
    if (!in.is_open()) {
        return Status::IOError("Could not read archive file: " + archive_full_path);
    }

    uint64_t orig_size = 0;
    uint32_t orig_crc = 0;
    in.read(reinterpret_cast<char*>(&orig_size), sizeof(orig_size));
    in.read(reinterpret_cast<char*>(&orig_crc), sizeof(orig_crc));

    std::string decompressed_data(orig_size, '\0');
    in.read(decompressed_data.data(), orig_size);
    in.close();

    uint32_t calc_crc = ComputeCRC32C(reinterpret_cast<const uint8_t*>(decompressed_data.data()), orig_size);
    if (calc_crc != orig_crc) {
        return Status::Corruption("CRC32C checksum mismatch during WAL archive restore");
    }

    std::ofstream out(dst_wal_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Status::IOError("Failed to write restored WAL file: " + dst_wal_path);
    }
    out.write(decompressed_data.data(), orig_size);
    out.close();

    return Status::OK();
}

std::vector<ArchivedSegmentMeta> ContinuousWALArchiver::GetArchivedSegments() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return segments_;
}

std::vector<ArchivedSegmentMeta> ContinuousWALArchiver::GetSegmentsCoveringRange(int64_t start_ts, int64_t target_ts) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ArchivedSegmentMeta> result;
    for (const auto& s : segments_) {
        // Overlap condition: segment_min <= target_ts and segment_max >= start_ts
        if (s.min_timestamp_micros <= target_ts && s.max_timestamp_micros >= start_ts) {
            result.push_back(s);
        }
    }
    return result;
}

Status ContinuousWALArchiver::SaveArchiveIndex() {
    std::string idx_path = (fs::path(archive_dir_) / "ARCHIVE_INDEX").string();
    std::ofstream out(idx_path, std::ios::trunc);
    if (!out.is_open()) {
        return Status::IOError("Failed to write ARCHIVE_INDEX");
    }

    for (const auto& s : segments_) {
        out << s.segment_id << " "
            << s.archive_path << " "
            << s.min_seq << " "
            << s.max_seq << " "
            << s.min_timestamp_micros << " "
            << s.max_timestamp_micros << " "
            << s.raw_size << " "
            << s.compressed_size << " "
            << s.crc32c << " "
            << s.sha256_hex << "\n";
    }
    return Status::OK();
}

Status ContinuousWALArchiver::LoadArchiveIndex() {
    std::string idx_path = (fs::path(archive_dir_) / "ARCHIVE_INDEX").string();
    std::ifstream in(idx_path);
    if (!in.is_open()) return Status::OK();

    segments_.clear();
    ArchivedSegmentMeta s;
    while (in >> s.segment_id
              >> s.archive_path
              >> s.min_seq
              >> s.max_seq
              >> s.min_timestamp_micros
              >> s.max_timestamp_micros
              >> s.raw_size
              >> s.compressed_size
              >> s.crc32c
              >> s.sha256_hex) {
        segments_.push_back(s);
    }
    return Status::OK();
}

} // namespace lsm
