#include "version_edit.hpp"
#include <sstream>

namespace lsm {

namespace {

// Helper Varint Encoding & Decoding routines
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

} // anonymous namespace

void VersionEdit::Clear() {
    comparator_.clear();
    log_number_ = 0;
    has_comparator_ = false;
    has_log_number_ = false;
    has_next_file_number_ = false;
    has_last_sequence_ = false;
    compact_pointers_.clear();
    deleted_files_.clear();
    new_files_.clear();
}

void VersionEdit::EncodeTo(std::string* dst) const {
    if (has_comparator_) {
        EncodeVarint32(dst, static_cast<uint32_t>(Tag::kComparator));
        EncodeLengthPrefixedSlice(dst, Slice(comparator_));
    }
    if (has_log_number_) {
        EncodeVarint32(dst, static_cast<uint32_t>(Tag::kLogNumber));
        EncodeVarint64(dst, log_number_);
    }
    if (has_next_file_number_) {
        EncodeVarint32(dst, static_cast<uint32_t>(Tag::kNextFileNumber));
        EncodeVarint64(dst, next_file_number_);
    }
    if (has_last_sequence_) {
        EncodeVarint32(dst, static_cast<uint32_t>(Tag::kLastSequence));
        EncodeVarint64(dst, last_sequence_);
    }

    for (const auto& cp : compact_pointers_) {
        EncodeVarint32(dst, static_cast<uint32_t>(Tag::kCompactPointer));
        EncodeVarint32(dst, static_cast<uint32_t>(cp.first));
        EncodeLengthPrefixedSlice(dst, cp.second.Encode());
    }

    for (const auto& df : deleted_files_) {
        EncodeVarint32(dst, static_cast<uint32_t>(Tag::kDeletedFile));
        EncodeVarint32(dst, static_cast<uint32_t>(df.first));
        EncodeVarint64(dst, df.second);
    }

    for (const auto& nf : new_files_) {
        const FileMetaData& f = nf.second;
        EncodeVarint32(dst, static_cast<uint32_t>(Tag::kNewFile));
        EncodeVarint32(dst, static_cast<uint32_t>(nf.first));
        EncodeVarint64(dst, f.file_number);
        EncodeVarint64(dst, f.file_size);
        EncodeLengthPrefixedSlice(dst, Slice(f.smallest_key));
        EncodeLengthPrefixedSlice(dst, Slice(f.largest_key));
    }
}

Status VersionEdit::DecodeFrom(const Slice& src) {
    Clear();
    Slice input = src;
    uint32_t tag = 0;

    while (DecodeVarint32(&input, &tag)) {
        switch (static_cast<Tag>(tag)) {
            case Tag::kComparator: {
                Slice str;
                if (DecodeLengthPrefixedSlice(&input, &str)) {
                    has_comparator_ = true;
                    comparator_ = str.ToString();
                } else {
                    return Status::Corruption("VersionEdit: Corrupt comparator name");
                }
                break;
            }
            case Tag::kLogNumber: {
                uint64_t v = 0;
                if (DecodeVarint64(&input, &v)) {
                    has_log_number_ = true;
                    log_number_ = v;
                } else {
                    return Status::Corruption("VersionEdit: Corrupt log number");
                }
                break;
            }
            case Tag::kNextFileNumber: {
                uint64_t v = 0;
                if (DecodeVarint64(&input, &v)) {
                    has_next_file_number_ = true;
                    next_file_number_ = v;
                } else {
                    return Status::Corruption("VersionEdit: Corrupt next file number");
                }
                break;
            }
            case Tag::kLastSequence: {
                uint64_t v = 0;
                if (DecodeVarint64(&input, &v)) {
                    has_last_sequence_ = true;
                    last_sequence_ = v;
                } else {
                    return Status::Corruption("VersionEdit: Corrupt last sequence");
                }
                break;
            }
            case Tag::kCompactPointer: {
                uint32_t level = 0;
                Slice key;
                if (DecodeVarint32(&input, &level) && DecodeLengthPrefixedSlice(&input, &key)) {
                    InternalKey ikey;
                    ikey.DecodeFrom(key);
                    compact_pointers_.push_back({static_cast<int>(level), ikey});
                } else {
                    return Status::Corruption("VersionEdit: Corrupt compact pointer");
                }
                break;
            }
            case Tag::kDeletedFile: {
                uint32_t level = 0;
                uint64_t file_num = 0;
                if (DecodeVarint32(&input, &level) && DecodeVarint64(&input, &file_num)) {
                    deleted_files_.insert({static_cast<int>(level), file_num});
                } else {
                    return Status::Corruption("VersionEdit: Corrupt deleted file record");
                }
                break;
            }
            case Tag::kNewFile: {
                uint32_t level = 0;
                uint64_t file_num = 0;
                uint64_t file_size = 0;
                Slice smallest, largest;
                if (DecodeVarint32(&input, &level) &&
                    DecodeVarint64(&input, &file_num) &&
                    DecodeVarint64(&input, &file_size) &&
                    DecodeLengthPrefixedSlice(&input, &smallest) &&
                    DecodeLengthPrefixedSlice(&input, &largest)) {
                    FileMetaData f;
                    f.file_number = file_num;
                    f.file_size = file_size;
                    f.smallest_key = smallest.ToString();
                    f.largest_key = largest.ToString();
                    new_files_.push_back({static_cast<int>(level), std::move(f)});
                } else {
                    return Status::Corruption("VersionEdit: Corrupt new file record");
                }
                break;
            }
            default:
                return Status::Corruption("VersionEdit: Unknown tag " + std::to_string(tag));
        }
    }

    return Status::OK();
}

std::string VersionEdit::DebugString() const {
    std::ostringstream oss;
    oss << "VersionEdit { ";
    if (has_comparator_) oss << "Comparator: " << comparator_ << ", ";
    if (has_log_number_) oss << "LogNumber: " << log_number_ << ", ";
    if (has_next_file_number_) oss << "NextFileNumber: " << next_file_number_ << ", ";
    if (has_last_sequence_) oss << "LastSequence: " << last_sequence_ << ", ";
    oss << "DeletedFiles: " << deleted_files_.size() << ", ";
    oss << "NewFiles: " << new_files_.size() << " }";
    return oss.str();
}

} // namespace lsm
