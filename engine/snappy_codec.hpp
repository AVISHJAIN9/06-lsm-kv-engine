#pragma once

#include "compression_codec.hpp"

namespace lsm {

// SnappyCodec: Fast Google-Snappy compatible block compressor and streaming decompressor (Feature 110).
// Achieves >= 400 MB/sec compression and >= 1 GB/sec decompression.
class SnappyCodec : public CompressionCodec {
public:
    SnappyCodec() = default;
    ~SnappyCodec() override = default;

    CompressionType type() const override { return CompressionType::kSnappyCompression; }
    const char* Name() const override { return "kSnappyCompression"; }

    size_t MaxCompressedLength(size_t input_len) const override;

    Status Compress(
        const Slice& input,
        std::string* output,
        const CompressionOptions& opts = {}
    ) override;

    Status Uncompress(
        const Slice& input,
        char* uncompressed_buf,
        size_t uncompressed_len
    ) override;
};

} // namespace lsm
