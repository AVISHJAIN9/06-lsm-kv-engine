#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include "slice.hpp"
#include "status.hpp"

namespace lsm {

// Compression algorithm identifiers matching SSTable block trailer byte tags (Feature 109).
enum class CompressionType : uint8_t {
    kNoCompression      = 0x0,
    kSnappyCompression  = 0x1,
    kZlibCompression    = 0x2,
    kBZip2Compression   = 0x3,
    kLZ4Compression     = 0x4,
    kLZ4HCCompression   = 0x5,
    kXpressCompression  = 0x6,
    kZSTD               = 0x7
};

struct CompressionOptions {
    int level{0}; // 0 = codec default
    bool use_dictionary{false};
    uint64_t dict_id{0};
    const void* dict_context{nullptr};
};

// CompressionCodec: Unified zero-copy abstract interface for pluggable block compressors (Feature 109).
class CompressionCodec {
public:
    virtual ~CompressionCodec() = default;

    virtual CompressionType type() const = 0;
    virtual const char* Name() const = 0;

    // Calculates conservative upper bound on compressed buffer size
    virtual size_t MaxCompressedLength(size_t input_len) const = 0;

    // Compresses input slice into output buffer (<= 2 ns dispatch overhead)
    virtual Status Compress(
        const Slice& input,
        std::string* output,
        const CompressionOptions& opts = {}
    ) = 0;

    // Zero-heap uncompress into pre-allocated destination buffer
    virtual Status Uncompress(
        const Slice& input,
        char* uncompressed_buf,
        size_t uncompressed_len
    ) = 0;

    // Convenience overload uncompressing into std::string
    virtual Status Uncompress(const Slice& input, std::string* output, size_t uncompressed_len) {
        if (!output) return Status::InvalidArgument("Null output string buffer");
        output->resize(uncompressed_len);
        if (uncompressed_len == 0) return Status::OK();
        return Uncompress(input, &(*output)[0], uncompressed_len);
    }
};

// NoCompressionCodec: Pass-through raw codec
class NoCompressionCodec : public CompressionCodec {
public:
    CompressionType type() const override { return CompressionType::kNoCompression; }
    const char* Name() const override { return "kNoCompression"; }

    size_t MaxCompressedLength(size_t input_len) const override { return input_len; }

    Status Compress(
        const Slice& input,
        std::string* output,
        const CompressionOptions& opts = {}
    ) override {
        (void)opts;
        if (!output) return Status::InvalidArgument("Null output");
        output->assign(input.data(), input.size());
        return Status::OK();
    }

    Status Uncompress(
        const Slice& input,
        char* uncompressed_buf,
        size_t uncompressed_len
    ) override {
        if (input.size() != uncompressed_len) {
            return Status::Corruption("NoCompression: Length mismatch");
        }
        if (uncompressed_len > 0 && uncompressed_buf) {
            std::memcpy(uncompressed_buf, input.data(), uncompressed_len);
        }
        return Status::OK();
    }
};

// CodecRegistry: Thread-safe singleton registering and resolving compression codecs (Feature 109).
class CodecRegistry {
public:
    static CodecRegistry& Instance();

    void Register(CompressionType type, std::shared_ptr<CompressionCodec> codec);
    std::shared_ptr<CompressionCodec> GetCodec(CompressionType type) const;

    bool IsSupported(CompressionType type) const;

private:
    CodecRegistry();
    mutable std::mutex mutex_{};
    std::unordered_map<uint8_t, std::shared_ptr<CompressionCodec>> codecs_{};
};

} // namespace lsm
