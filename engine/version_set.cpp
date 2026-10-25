#include "version_set.hpp"
#include "current_file.hpp"
#include "wal_format.hpp"
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace lsm {

VersionSet::VersionSet(const std::string& dbname, const std::string& comparator_name)
    : dbname_(dbname), comparator_name_(comparator_name), dummy_versions_(this) {
    // Append initial empty base version
    auto* base = new Version(this);
    AppendVersion(base);
}

VersionSet::~VersionSet() {
    if (current_) {
        current_->Unref();
        current_ = nullptr;
    }
    if (manifest_file_.is_open()) {
        manifest_file_.close();
    }
}

void VersionSet::AppendVersion(Version* v) {
    if (!v) return;

    // Make 'v' current
    if (current_) {
        current_->Unref();
    }
    current_ = v;
    v->Ref();

    // Link 'v' before dummy_versions_
    v->prev_ = dummy_versions_.prev_;
    v->next_ = &dummy_versions_;
    v->prev_->next_ = v;
    v->next_->prev_ = v;
}

Status VersionSet::WriteSnapshotToManifest(const std::string& manifest_path) {
    manifest_file_.open(manifest_path, std::ios::binary | std::ios::out | std::ios::app);
    if (!manifest_file_.is_open()) {
        return Status::IOError("Failed to open MANIFEST file: " + manifest_path);
    }

    // Write baseline snapshot VersionEdit
    VersionEdit edit;
    edit.SetComparatorName(Slice(comparator_name_));
    edit.SetNextFile(next_file_number_);
    edit.SetLastSequence(last_sequence_);
    edit.SetLogNumber(log_number_);

    if (current_) {
        for (int l = 0; l < Version::kNumLevels; ++l) {
            for (const auto& f : current_->files(l)) {
                edit.AddFile(l, f);
            }
            if (!current_->compact_pointer(l).empty()) {
                InternalKey cp_key;
                cp_key.DecodeFrom(Slice(current_->compact_pointer(l)));
                edit.SetCompactPointer(l, cp_key);
            }
        }
    }

    std::string record;
    edit.EncodeTo(&record);
    return AppendRecordToManifest(record);
}

Status VersionSet::AppendRecordToManifest(const std::string& record) {
    if (!manifest_file_.is_open()) {
        return Status::IOError("Manifest file stream is not open");
    }

    // Binary framing: [CRC32C: 4 bytes] [Length: 2 bytes] [RecordType: 1 byte] [Payload]
    uint32_t crc = wal::CRC32C::Value(record.data(), record.size());
    crc = wal::CRC32C::Mask(crc);
    uint16_t length = static_cast<uint16_t>(record.size());
    uint8_t type = 1; // Full record

    char header[7];
    for (int i = 0; i < 4; ++i) {
        header[i] = static_cast<char>((crc >> (i * 8)) & 0xFF);
    }
    header[4] = static_cast<char>(length & 0xFF);
    header[5] = static_cast<char>((length >> 8) & 0xFF);
    header[6] = static_cast<char>(type);

    manifest_file_.write(header, 7);
    manifest_file_.write(record.data(), record.size());
    manifest_file_.flush();

    manifest_bytes_written_ += (7 + record.size());
    return Status::OK();
}

Status VersionSet::LogAndApply(VersionEdit* edit, std::mutex* /*mu*/) {
    if (!edit) {
        return Status::InvalidArgument("Null VersionEdit provided to LogAndApply");
    }

    if (edit->has_log_number_) {
        log_number_ = edit->log_number_;
    }
    if (edit->has_next_file_number_) {
        next_file_number_ = edit->next_file_number_;
    }
    if (edit->has_last_sequence_) {
        last_sequence_ = edit->last_sequence_;
    }

    // 1. Build new Version
    auto* v = new Version(this);
    VersionBuilder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);

    // 2. Open manifest file if not yet opened
    if (!manifest_file_.is_open()) {
        std::ostringstream oss;
        oss << dbname_ << "/MANIFEST-" << std::setw(6) << std::setfill('0') << manifest_file_number_;
        std::string manifest_path = oss.str();

        Status s = WriteSnapshotToManifest(manifest_path);
        if (!s.ok()) {
            delete v;
            return s;
        }

        s = SetCurrentFile(dbname_, manifest_file_number_);
        if (!s.ok()) {
            delete v;
            return s;
        }
    }

    // Encode and append delta record
    std::string record;
    edit->EncodeTo(&record);
    Status s = AppendRecordToManifest(record);
    if (!s.ok()) {
        delete v;
        return s;
    }

    // 3. Install new Version
    AppendVersion(v);
    return Status::OK();
}

Status VersionSet::Recover() {
    std::string manifest_filename;
    Status s = ReadCurrentFile(dbname_, &manifest_filename);
    if (!s.ok()) {
        return s;
    }

    std::string manifest_path = dbname_ + "/" + manifest_filename;
    std::ifstream file(manifest_path, std::ios::binary);
    if (!file.is_open()) {
        return Status::IOError("Failed to open manifest file during recovery: " + manifest_path);
    }

    VersionBuilder builder(this, current_);
    std::string buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Slice input(buffer);

    while (input.size() >= 7) {
        // Read header
        const uint8_t* p = reinterpret_cast<const uint8_t*>(input.data());
        uint32_t expected_crc = 0;
        for (int i = 0; i < 4; ++i) {
            expected_crc |= (static_cast<uint32_t>(p[i]) << (i * 8));
        }
        uint16_t length = static_cast<uint16_t>(p[4]) | (static_cast<uint16_t>(p[5]) << 8);
        input.remove_prefix(7);

        if (input.size() < length) {
            return Status::Corruption("Truncated manifest record");
        }

        Slice record(input.data(), length);
        input.remove_prefix(length);

        uint32_t actual_crc = wal::CRC32C::Value(record.data(), record.size());
        actual_crc = wal::CRC32C::Mask(actual_crc);
        if (actual_crc != expected_crc) {
            return Status::Corruption("Manifest record CRC32C checksum mismatch");
        }

        VersionEdit edit;
        s = edit.DecodeFrom(record);
        if (!s.ok()) {
            return s;
        }

        if (edit.has_comparator_ && edit.comparator_ != comparator_name_) {
            return Status::InvalidArgument("Manifest comparator name mismatch: " + edit.comparator_);
        }
        if (edit.has_log_number_) log_number_ = edit.log_number_;
        if (edit.has_next_file_number_) next_file_number_ = edit.next_file_number_;
        if (edit.has_last_sequence_) last_sequence_ = edit.last_sequence_;

        builder.Apply(&edit);
    }

    auto* v = new Version(this);
    builder.SaveTo(v);
    AppendVersion(v);

    return Status::OK();
}

std::string VersionSet::Summary() const {
    std::ostringstream oss;
    oss << "VersionSet (NextFile=" << next_file_number_ << ", ManifestFile=" << manifest_file_number_
        << ", LastSeq=" << last_sequence_ << ", LogNum=" << log_number_ << "): ";
    if (current_) {
        oss << current_->DebugString();
    }
    return oss.str();
}

} // namespace lsm
