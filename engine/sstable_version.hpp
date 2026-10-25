#pragma once

#include <cstdint>
#include <string>
#include "status.hpp"
#include "format.hpp"

namespace lsm {

// -----------------------------------------------------------------------------
// TableFormatVersion & Feature Flag Bitsets
// -----------------------------------------------------------------------------
enum class TableFormatVersion : uint32_t {
    kFormatVersionV1 = 1, // Classic single-level index, standard footer
    kFormatVersionV2 = 2, // Two-level index, block-based Bloom, versioned footer
    kCurrentVersion = 2
};

namespace TableFeatureFlags {
    static constexpr uint32_t kFlagNone           = 0;
    static constexpr uint32_t kFlagTwoLevelIndex   = 1 << 0;
    static constexpr uint32_t kFlagBlockBloom      = 1 << 1;
    static constexpr uint32_t kFlagEncrypted       = 1 << 2;
    static constexpr uint32_t kFlagZSTD            = 1 << 3;
    static constexpr uint32_t kFlagDictionary      = 1 << 4;

    static constexpr uint32_t kSupportedFlagsMask  = (1 << 5) - 1; // All 5 flags
}

// -----------------------------------------------------------------------------
// TableVersionManager: Forward and backward compatibility validator
// -----------------------------------------------------------------------------
class TableVersionManager {
public:
    static bool IsSupportedVersion(uint32_t version) {
        return version >= static_cast<uint32_t>(TableFormatVersion::kFormatVersionV1) &&
               version <= static_cast<uint32_t>(TableFormatVersion::kFormatVersionV2);
    }

    static bool IsSupportedFlags(uint32_t flags) {
        return (flags & ~TableFeatureFlags::kSupportedFlagsMask) == 0;
    }

    static Status ValidateCompatibility(uint32_t version, uint32_t flags) {
        if (!IsSupportedVersion(version)) {
            return Status::NotSupported("Unsupported SSTable format version: v" + std::to_string(version));
        }
        if (!IsSupportedFlags(flags)) {
            return Status::NotSupported("Unsupported SSTable feature flags bitset: 0x" + std::to_string(flags));
        }
        return Status::OK();
    }
};

// -----------------------------------------------------------------------------
// VersionedFooter: Extended footer for V2 SSTable files
// Layout: [Metaindex Handle][Index Handle][Padding zeros][Version: 4B][Flags: 4B][Magic: 8B]
// Fixed Length = 48 bytes (same total physical size, keeping backward compatibility)
// -----------------------------------------------------------------------------
class VersionedFooter {
public:
    static constexpr uint64_t kTableMagicNumber = Footer::kTableMagicNumber;
    static constexpr size_t kEncodedLength = Footer::kEncodedLength; // 48 bytes

    VersionedFooter() = default;
    VersionedFooter(
        BlockHandle metaindex_handle,
        BlockHandle index_handle,
        uint32_t version = static_cast<uint32_t>(TableFormatVersion::kCurrentVersion),
        uint32_t flags = TableFeatureFlags::kFlagNone
    ) : metaindex_handle_(metaindex_handle),
        index_handle_(index_handle),
        version_(version),
        flags_(flags) {}

    const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
    void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

    const BlockHandle& index_handle() const { return index_handle_; }
    void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

    uint32_t version() const { return version_; }
    void set_version(uint32_t v) { version_ = v; }

    uint32_t flags() const { return flags_; }
    void set_flags(uint32_t f) { flags_ = f; }

    void EncodeTo(std::string* dst) const {
        const size_t orig_size = dst->size();
        metaindex_handle_.EncodeTo(dst);
        index_handle_.EncodeTo(dst);

        // Pad with zeros up to 32 bytes (2 * BlockHandle::kMaxEncodedLength - 8)
        dst->resize(orig_size + 32, 0);

        // Append Version (4 bytes fixed32) and Flags (4 bytes fixed32)
        char vf_buf[8];
        vf_buf[0] = static_cast<char>(version_ & 0xff);
        vf_buf[1] = static_cast<char>((version_ >> 8) & 0xff);
        vf_buf[2] = static_cast<char>((version_ >> 16) & 0xff);
        vf_buf[3] = static_cast<char>((version_ >> 24) & 0xff);

        vf_buf[4] = static_cast<char>(flags_ & 0xff);
        vf_buf[5] = static_cast<char>((flags_ >> 8) & 0xff);
        vf_buf[6] = static_cast<char>((flags_ >> 16) & 0xff);
        vf_buf[7] = static_cast<char>((flags_ >> 24) & 0xff);
        dst->append(vf_buf, 8);

        // Append 8-byte magic number
        char magic_buf[8];
        magic_buf[0] = static_cast<char>(kTableMagicNumber & 0xff);
        magic_buf[1] = static_cast<char>((kTableMagicNumber >> 8) & 0xff);
        magic_buf[2] = static_cast<char>((kTableMagicNumber >> 16) & 0xff);
        magic_buf[3] = static_cast<char>((kTableMagicNumber >> 24) & 0xff);
        magic_buf[4] = static_cast<char>((kTableMagicNumber >> 32) & 0xff);
        magic_buf[5] = static_cast<char>((kTableMagicNumber >> 40) & 0xff);
        magic_buf[6] = static_cast<char>((kTableMagicNumber >> 48) & 0xff);
        magic_buf[7] = static_cast<char>((kTableMagicNumber >> 56) & 0xff);
        dst->append(magic_buf, 8);
    }

    Status DecodeFrom(Slice* input) {
        if (input->size() < kEncodedLength) {
            return Status::Corruption("Input too short to contain a valid 48-byte SSTable footer");
        }

        const char* magic_ptr = input->data() + kEncodedLength - 8;
        uint64_t magic = (static_cast<uint64_t>(static_cast<uint8_t>(magic_ptr[0]))) |
                         (static_cast<uint64_t>(static_cast<uint8_t>(magic_ptr[1])) << 8) |
                         (static_cast<uint64_t>(static_cast<uint8_t>(magic_ptr[2])) << 16) |
                         (static_cast<uint64_t>(static_cast<uint8_t>(magic_ptr[3])) << 24) |
                         (static_cast<uint64_t>(static_cast<uint8_t>(magic_ptr[4])) << 32) |
                         (static_cast<uint64_t>(static_cast<uint8_t>(magic_ptr[5])) << 40) |
                         (static_cast<uint64_t>(static_cast<uint8_t>(magic_ptr[6])) << 48) |
                         (static_cast<uint64_t>(static_cast<uint8_t>(magic_ptr[7])) << 56);

        if (magic != kTableMagicNumber) {
            return Status::Corruption("Invalid SSTable footer magic constant");
        }

        // Decode Version and Flags from offset 32..40
        const char* vf_ptr = input->data() + 32;
        version_ = (static_cast<uint32_t>(static_cast<uint8_t>(vf_ptr[0]))) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(vf_ptr[1])) << 8) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(vf_ptr[2])) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(vf_ptr[3])) << 24);

        flags_ = (static_cast<uint32_t>(static_cast<uint8_t>(vf_ptr[4]))) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(vf_ptr[5])) << 8) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(vf_ptr[6])) << 16) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(vf_ptr[7])) << 24);

        Status compat_s = TableVersionManager::ValidateCompatibility(version_, flags_);
        if (!compat_s.ok()) {
            return compat_s;
        }

        Slice handles_slice(input->data(), 32);
        Status s = metaindex_handle_.DecodeFrom(&handles_slice);
        if (!s.ok()) return s;

        s = index_handle_.DecodeFrom(&handles_slice);
        if (!s.ok()) return s;

        *input = Slice(input->data() + kEncodedLength, input->size() - kEncodedLength);
        return Status::OK();
    }

private:
    BlockHandle metaindex_handle_;
    BlockHandle index_handle_;
    uint32_t version_{static_cast<uint32_t>(TableFormatVersion::kCurrentVersion)};
    uint32_t flags_{TableFeatureFlags::kFlagNone};
};

} // namespace lsm
