#pragma once

#include <vector>
#include <string>
#include <memory>
#include "compression_codec.hpp"

// Forward declare ZSTD context structures to avoid exposing raw pointers in public headers
typedef struct ZSTD_CCtx_s ZSTD_CCtx;
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
typedef struct ZSTD_CDict_s ZSTD_CDict;
typedef struct ZSTD_DDict_s ZSTD_DDict;

namespace lsm {

// ZSTDDictionary: Manages pre-trained dictionary contexts for high-ratio small-record compression (Feature 111).
class ZSTDDictionary {
public:
    ZSTDDictionary();
    ~ZSTDDictionary();

    ZSTDDictionary(const ZSTDDictionary&) = delete;
    ZSTDDictionary& operator=(const ZSTDDictionary&) = delete;

    // Trains dictionary using sample key-value records (e.g., collected from memtables)
    Status Train(const std::vector<std::string>& samples, size_t dict_capacity = 32 * 1024);

    // Loads pre-existing dictionary binary buffer
    Status LoadFromBuffer(const Slice& dict_buffer, int compression_level = 3);

    uint32_t GetDictID() const { return dict_id_; }
    const std::string& GetDictBuffer() const { return dict_buffer_; }

    const ZSTD_CDict* GetCDict() const { return cdict_; }
    const ZSTD_DDict* GetDDict() const { return ddict_; }

private:
    uint32_t dict_id_{0};
    std::string dict_buffer_{};
    ZSTD_CDict* cdict_{nullptr};
    ZSTD_DDict* ddict_{nullptr};
};

// ZSTDCodec: High-ratio Zstandard compression provider with cached contexts and dictionary support (Feature 111).
class ZSTDCodec : public CompressionCodec {
public:
    explicit ZSTDCodec(int default_level = 3);
    ~ZSTDCodec() override = default;

    CompressionType type() const override { return CompressionType::kZSTD; }
    const char* Name() const override { return "kZSTD"; }

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

    Status CompressWithDict(
        const Slice& input,
        std::string* output,
        const ZSTDDictionary* dict,
        int level = 3
    );

    Status UncompressWithDict(
        const Slice& input,
        char* uncompressed_buf,
        size_t uncompressed_len,
        const ZSTDDictionary* dict
    );

private:
    int default_level_{3};
};

} // namespace lsm
