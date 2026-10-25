#pragma once

#include "compression_codec.hpp"

namespace lsm {

// LZ4Codec: Ultra-high-throughput real-time block compressor (Feature 112).
// Delivers decompression speeds exceeding 2.5 GB/sec with bounded memory guarantees.
class LZ4Codec : public CompressionCodec {
public:
    LZ4Codec() = default;
    ~LZ4Codec() override = default;

    CompressionType type() const override { return CompressionType::kLZ4Compression; }
    const char* Name() const override { return "kLZ4Compression"; }

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
