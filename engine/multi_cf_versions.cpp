#include "multi_cf_versions.hpp"
#include "wal_format.hpp"
#include "current_file.hpp"
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

namespace lsm {

namespace {

void EncodeVarint32(std::string* dst, uint32_t v) {
    char buf[5];
    char* ptr = buf;
    while (v >= 128) {
        *(ptr++) = static_cast<char>((v & 127) | 128);
        v >>= 7;
    }
    *(ptr++) = static_cast<char>(v);
    dst->append(buf, ptr - buf);
}

void EncodeVarint64(std::string* dst, uint64_t v) {
    char buf[10];
    char* ptr = buf;
    while (v >= 128) {
        *(ptr++) = static_cast<char>((v & 127) | 128);
        v >>= 7;
    }
    *(ptr++) = static_cast<char>(v);
    dst->append(buf, ptr - buf);
}

void EncodeLengthPrefixedSlice(std::string* dst, const Slice& s) {
    EncodeVarint32(dst, static_cast<uint32_t>(s.size()));
    dst->append(s.data(), s.size());
}

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

bool DecodeVarint64(Slice* input, uint64_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    uint64_t result = 0;
    for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
        uint64_t byte = static_cast<uint8_t>(*p++);
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

std::string MakeManifestFileName(const std::string& dbname, uint64_t number) {
    std::ostringstream oss;
    oss << dbname << "/MANIFEST-" << std::setw(6) << std::setfill('0') << number;
    return oss.str();
}

} // anonymous namespace

void MultiCFVersionEdit::Clear() {
    added_cfs_.clear();
    dropped_cfs_.clear();
    new_files_.clear();
    deleted_files_.clear();
    has_last_sequence_ = false;
    last_sequence_ = 0;
    has_log_number_ = false;
    log_number_ = 0;
    has_next_file_number_ = false;
    next_file_number_ = 0;
}

void MultiCFVersionEdit::AddColumnFamily(uint32_t cf_id, const std::string& name) {
    added_cfs_.push_back({cf_id, name});
}

void MultiCFVersionEdit::DropColumnFamily(uint32_t cf_id) {
    dropped_cfs_.push_back({cf_id});
}

void MultiCFVersionEdit::AddFile(uint32_t cf_id, int level, const FileMetaData& meta) {
    new_files_.push_back({cf_id, level, meta});
}

void MultiCFVersionEdit::DeleteFile(uint32_t cf_id, int level, uint64_t file_number) {
    deleted_files_.push_back({cf_id, level, file_number});
}

void MultiCFVersionEdit::EncodeTo(std::string* dst) const {
    if (has_log_number_) {
        EncodeVarint32(dst, static_cast<uint32_t>(MultiCFTag::kLogNumber));
        EncodeVarint64(dst, log_number_);
    }
    if (has_next_file_number_) {
        EncodeVarint32(dst, static_cast<uint32_t>(MultiCFTag::kNextFileNumber));
        EncodeVarint64(dst, next_file_number_);
    }
    if (has_last_sequence_) {
        EncodeVarint32(dst, static_cast<uint32_t>(MultiCFTag::kLastSequence));
        EncodeVarint64(dst, last_sequence_);
    }

    for (const auto& add_cf : added_cfs_) {
        EncodeVarint32(dst, static_cast<uint32_t>(MultiCFTag::kAddColumnFamily));
        EncodeVarint32(dst, add_cf.cf_id);
        EncodeLengthPrefixedSlice(dst, Slice(add_cf.cf_name));
    }

    for (const auto& drop_cf : dropped_cfs_) {
        EncodeVarint32(dst, static_cast<uint32_t>(MultiCFTag::kDropColumnFamily));
        EncodeVarint32(dst, drop_cf.cf_id);
    }

    for (const auto& df : deleted_files_) {
        EncodeVarint32(dst, static_cast<uint32_t>(MultiCFTag::kDeleteFile));
        EncodeVarint32(dst, df.cf_id);
        EncodeVarint32(dst, static_cast<uint32_t>(df.level));
        EncodeVarint64(dst, df.file_number);
    }

    for (const auto& nf : new_files_) {
        const FileMetaData& f = nf.file_meta;
        EncodeVarint32(dst, static_cast<uint32_t>(MultiCFTag::kAddFile));
        EncodeVarint32(dst, nf.cf_id);
        EncodeVarint32(dst, static_cast<uint32_t>(nf.level));
        EncodeVarint64(dst, f.file_number);
        EncodeVarint64(dst, f.file_size);
        EncodeLengthPrefixedSlice(dst, Slice(f.smallest_key));
        EncodeLengthPrefixedSlice(dst, Slice(f.largest_key));
    }
}

Status MultiCFVersionEdit::DecodeFrom(const Slice& src) {
    Clear();
    Slice input = src;
    uint32_t tag = 0;

    while (DecodeVarint32(&input, &tag)) {
        switch (static_cast<MultiCFTag>(tag)) {
            case MultiCFTag::kLogNumber: {
                uint64_t v = 0;
                if (DecodeVarint64(&input, &v)) {
                    has_log_number_ = true;
                    log_number_ = v;
                } else {
                    return Status::Corruption("MultiCFVersionEdit: Corrupt log number");
                }
                break;
            }
            case MultiCFTag::kNextFileNumber: {
                uint64_t v = 0;
                if (DecodeVarint64(&input, &v)) {
                    has_next_file_number_ = true;
                    next_file_number_ = v;
                } else {
                    return Status::Corruption("MultiCFVersionEdit: Corrupt next file number");
                }
                break;
            }
            case MultiCFTag::kLastSequence: {
                uint64_t v = 0;
                if (DecodeVarint64(&input, &v)) {
                    has_last_sequence_ = true;
                    last_sequence_ = v;
                } else {
                    return Status::Corruption("MultiCFVersionEdit: Corrupt last sequence");
                }
                break;
            }
            case MultiCFTag::kAddColumnFamily: {
                uint32_t cf_id = 0;
                Slice cf_name;
                if (DecodeVarint32(&input, &cf_id) && DecodeLengthPrefixedSlice(&input, &cf_name)) {
                    added_cfs_.push_back({cf_id, cf_name.ToString()});
                } else {
                    return Status::Corruption("MultiCFVersionEdit: Corrupt AddColumnFamily");
                }
                break;
            }
            case MultiCFTag::kDropColumnFamily: {
                uint32_t cf_id = 0;
                if (DecodeVarint32(&input, &cf_id)) {
                    dropped_cfs_.push_back({cf_id});
                } else {
                    return Status::Corruption("MultiCFVersionEdit: Corrupt DropColumnFamily");
                }
                break;
            }
            case MultiCFTag::kDeleteFile: {
                uint32_t cf_id = 0;
                uint32_t level = 0;
                uint64_t file_num = 0;
                if (DecodeVarint32(&input, &cf_id) &&
                    DecodeVarint32(&input, &level) &&
                    DecodeVarint64(&input, &file_num)) {
                    deleted_files_.push_back({cf_id, static_cast<int>(level), file_num});
                } else {
                    return Status::Corruption("MultiCFVersionEdit: Corrupt DeleteFile");
                }
                break;
            }
            case MultiCFTag::kAddFile: {
                uint32_t cf_id = 0;
                uint32_t level = 0;
                uint64_t file_num = 0;
                uint64_t file_size = 0;
                Slice smallest, largest;
                if (DecodeVarint32(&input, &cf_id) &&
                    DecodeVarint32(&input, &level) &&
                    DecodeVarint64(&input, &file_num) &&
                    DecodeVarint64(&input, &file_size) &&
                    DecodeLengthPrefixedSlice(&input, &smallest) &&
                    DecodeLengthPrefixedSlice(&input, &largest)) {
                    FileMetaData f;
                    f.file_number = file_num;
                    f.file_size = file_size;
                    f.smallest_key = smallest.ToString();
                    f.largest_key = largest.ToString();
                    new_files_.push_back({cf_id, static_cast<int>(level), std::move(f)});
                } else {
                    return Status::Corruption("MultiCFVersionEdit: Corrupt AddFile");
                }
                break;
            }
            default:
                return Status::Corruption("MultiCFVersionEdit: Unknown tag " + std::to_string(tag));
        }
    }

    return Status::OK();
}

MultiCFVersionSet::MultiCFVersionSet(const std::string& dbname)
    : dbname_(dbname) {
    std::filesystem::create_directories(dbname_);
}

MultiCFVersionSet::~MultiCFVersionSet() {
    if (manifest_file_.is_open()) {
        manifest_file_.flush();
        manifest_file_.close();
    }
}

Status MultiCFVersionSet::CreateNewManifest(uint64_t manifest_num) {
    std::lock_guard<std::mutex> lock(manifest_mutex_);
    manifest_file_number_ = manifest_num;
    std::string path = MakeManifestFileName(dbname_, manifest_num);
    if (manifest_file_.is_open()) {
        manifest_file_.close();
    }
    manifest_file_.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!manifest_file_.is_open()) {
        return Status::IOError("Failed to open MANIFEST file: " + path);
    }

    Status s = SetCurrentFile(dbname_, manifest_num);
    return s;
}

Status MultiCFVersionSet::AppendRecordToManifest(const std::string& record) {
    if (!manifest_file_.is_open()) {
        Status s = CreateNewManifest(manifest_file_number_);
        if (!s.ok()) return s;
    }

    uint32_t length = static_cast<uint32_t>(record.size());
    uint32_t crc = wal::CRC32C::Value(record.data(), record.size());
    crc = wal::CRC32C::Mask(crc);

    char header[8];
    header[0] = static_cast<char>(length & 0xff);
    header[1] = static_cast<char>((length >> 8) & 0xff);
    header[2] = static_cast<char>((length >> 16) & 0xff);
    header[3] = static_cast<char>((length >> 24) & 0xff);

    header[4] = static_cast<char>(crc & 0xff);
    header[5] = static_cast<char>((crc >> 8) & 0xff);
    header[6] = static_cast<char>((crc >> 16) & 0xff);
    header[7] = static_cast<char>((crc >> 24) & 0xff);

    manifest_file_.write(header, 8);
    manifest_file_.write(record.data(), record.size());
    manifest_file_.flush();

    if (!manifest_file_.good()) {
        return Status::IOError("Failed to write record to MANIFEST");
    }

    return Status::OK();
}

Status MultiCFVersionSet::LogAndApply(
    const std::vector<MultiCFVersionEdit>& edits,
    ColumnFamilySet* cf_set,
    std::mutex* mu
) {
    if (edits.empty()) return Status::OK();

    std::unique_lock<std::mutex> caller_lock;
    if (mu) {
        caller_lock = std::unique_lock<std::mutex>(*mu, std::adopt_lock);
    }

    std::lock_guard<std::mutex> lock(manifest_mutex_);

    // 1. Serialize all edits into a single contiguous record payload
    std::string payload;
    for (const auto& edit : edits) {
        std::string edit_bytes;
        edit.EncodeTo(&edit_bytes);
        EncodeLengthPrefixedSlice(&payload, Slice(edit_bytes));
    }

    // 2. Commit record to MANIFEST log and flush to disk
    Status s = AppendRecordToManifest(payload);
    if (!s.ok()) {
        if (mu) caller_lock.release();
        return s;
    }

    // 3. Apply state transitions to memory across target CFs
    if (cf_set) {
        for (const auto& edit : edits) {
            // Apply additions/drops of CFs
            for (const auto& add : edit.added_cfs()) {
                if (!cf_set->GetColumnFamily(add.cf_id)) {
                    cf_set->CreateColumnFamily(add.cf_name);
                }
            }
            for (const auto& drop : edit.dropped_cfs()) {
                auto h = cf_set->GetColumnFamily(drop.cf_id);
                if (h && h->cfd()) {
                    cf_set->DropColumnFamily(h.get());
                }
            }

            // Apply new files
            for (const auto& nf : edit.new_files()) {
                auto h = cf_set->GetColumnFamily(nf.cf_id);
                if (h && h->cfd()) {
                    h->cfd()->AddLevelFile(nf.level, nf.file_meta);
                }
            }

            if (edit.has_last_sequence()) {
                last_sequence_ = edit.last_sequence();
            }
            if (edit.has_next_file_number()) {
                next_file_number_ = edit.next_file_number();
            }
        }
    }

    if (mu) caller_lock.release();
    return Status::OK();
}

} // namespace lsm
