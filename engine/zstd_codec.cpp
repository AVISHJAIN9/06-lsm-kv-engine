#include "zstd_codec.hpp"
#include <zstd.h>
#include <zdict.h>
#include <vector>
#include <numeric>

namespace lsm {

namespace {

thread_local ZSTD_CCtx* tl_cctx = nullptr;
thread_local ZSTD_DCtx* tl_dctx = nullptr;

struct TLContextCleaner {
    ~TLContextCleaner() {
        if (tl_cctx) {
            ZSTD_freeCCtx(tl_cctx);
            tl_cctx = nullptr;
        }
        if (tl_dctx) {
            ZSTD_freeDCtx(tl_dctx);
            tl_dctx = nullptr;
        }
    }
};

thread_local TLContextCleaner tl_cleaner;

ZSTD_CCtx* GetThreadLocalCCtx() {
    if (!tl_cctx) {
        tl_cctx = ZSTD_createCCtx();
    }
    return tl_cctx;
}

ZSTD_DCtx* GetThreadLocalDCtx() {
    if (!tl_dctx) {
        tl_dctx = ZSTD_createDCtx();
    }
    return tl_dctx;
}

} // anonymous namespace

ZSTDDictionary::ZSTDDictionary() = default;

ZSTDDictionary::~ZSTDDictionary() {
    if (cdict_) {
        ZSTD_freeCDict(cdict_);
        cdict_ = nullptr;
    }
    if (ddict_) {
        ZSTD_freeDDict(ddict_);
        ddict_ = nullptr;
    }
}

Status ZSTDDictionary::Train(const std::vector<std::string>& samples, size_t dict_capacity) {
    if (samples.empty()) {
        return Status::InvalidArgument("Cannot train dictionary with empty samples");
    }

    size_t total_samples_bytes = 0;
    std::vector<size_t> sample_sizes;
    sample_sizes.reserve(samples.size());

    for (const auto& s : samples) {
        total_samples_bytes += s.size();
        sample_sizes.push_back(s.size());
    }

    std::string sample_buffer;
    sample_buffer.reserve(total_samples_bytes);
    for (const auto& s : samples) {
        sample_buffer.append(s);
    }

    dict_buffer_.resize(dict_capacity);
    size_t dict_size = ZDICT_trainFromBuffer(
        &dict_buffer_[0],
        dict_capacity,
        sample_buffer.data(),
        sample_sizes.data(),
        static_cast<unsigned>(sample_sizes.size())
    );

    if (ZDICT_isError(dict_size)) {
        dict_buffer_.clear();
        return Status::Corruption("ZSTD dictionary training failed: " +
                                  std::string(ZDICT_getErrorName(dict_size)));
    }

    dict_buffer_.resize(dict_size);
    dict_id_ = ZDICT_getDictID(dict_buffer_.data(), dict_size);
    if (dict_id_ == 0) {
        dict_id_ = 1001; // Fallback valid dictionary ID
    }

    return LoadFromBuffer(Slice(dict_buffer_), 3);
}

Status ZSTDDictionary::LoadFromBuffer(const Slice& dict_buffer, int compression_level) {
    if (cdict_) {
        ZSTD_freeCDict(cdict_);
        cdict_ = nullptr;
    }
    if (ddict_) {
        ZSTD_freeDDict(ddict_);
        ddict_ = nullptr;
    }

    dict_buffer_ = dict_buffer.ToString();
    if (dict_buffer_.empty()) {
        return Status::InvalidArgument("Empty dictionary buffer");
    }

    dict_id_ = ZDICT_getDictID(dict_buffer_.data(), dict_buffer_.size());
    if (dict_id_ == 0) {
        dict_id_ = 1001;
    }

    cdict_ = ZSTD_createCDict(dict_buffer_.data(), dict_buffer_.size(), compression_level);
    if (!cdict_) {
        return Status::Corruption("Failed to create ZSTD_CDict from buffer");
    }

    ddict_ = ZSTD_createDDict(dict_buffer_.data(), dict_buffer_.size());
    if (!ddict_) {
        ZSTD_freeCDict(cdict_);
        cdict_ = nullptr;
        return Status::Corruption("Failed to create ZSTD_DDict from buffer");
    }

    return Status::OK();
}

ZSTDCodec::ZSTDCodec(int default_level)
    : default_level_(default_level) {}

size_t ZSTDCodec::MaxCompressedLength(size_t input_len) const {
    return ZSTD_compressBound(input_len);
}

Status ZSTDCodec::Compress(
    const Slice& input,
    std::string* output,
    const CompressionOptions& opts
) {
    if (!output) return Status::InvalidArgument("Null output string buffer");

    if (input.size() == 0) {
        output->clear();
        return Status::OK();
    }

    if (opts.use_dictionary && opts.dict_context) {
        const auto* dict = static_cast<const ZSTDDictionary*>(opts.dict_context);
        return CompressWithDict(input, output, dict, opts.level > 0 ? opts.level : default_level_);
    }

    int level = opts.level > 0 ? opts.level : default_level_;
    size_t max_dst_size = ZSTD_compressBound(input.size());
    output->resize(max_dst_size);

    ZSTD_CCtx* cctx = GetThreadLocalCCtx();
    size_t comp_size = ZSTD_compressCCtx(
        cctx,
        &(*output)[0],
        max_dst_size,
        input.data(),
        input.size(),
        level
    );

    if (ZSTD_isError(comp_size)) {
        return Status::Corruption("ZSTD compression failed: " +
                                  std::string(ZSTD_getErrorName(comp_size)));
    }

    output->resize(comp_size);
    return Status::OK();
}

Status ZSTDCodec::Uncompress(
    const Slice& input,
    char* uncompressed_buf,
    size_t uncompressed_len
) {
    if (uncompressed_len == 0) return Status::OK();
    if (!uncompressed_buf) return Status::InvalidArgument("Null destination buffer");

    ZSTD_DCtx* dctx = GetThreadLocalDCtx();
    size_t res = ZSTD_decompressDCtx(
        dctx,
        uncompressed_buf,
        uncompressed_len,
        input.data(),
        input.size()
    );

    if (ZSTD_isError(res)) {
        return Status::Corruption("ZSTD decompression failed: " +
                                  std::string(ZSTD_getErrorName(res)));
    }

    if (res != uncompressed_len) {
        return Status::Corruption("ZSTD decompressed size mismatch");
    }

    return Status::OK();
}

Status ZSTDCodec::CompressWithDict(
    const Slice& input,
    std::string* output,
    const ZSTDDictionary* dict,
    int level
) {
    (void)level;
    if (!output) return Status::InvalidArgument("Null output buffer");
    if (!dict || !dict->GetCDict()) {
        CompressionOptions fallback_opts;
        return Compress(input, output, fallback_opts);
    }

    if (input.size() == 0) {
        output->clear();
        return Status::OK();
    }

    size_t max_dst_size = ZSTD_compressBound(input.size());
    output->resize(max_dst_size);

    ZSTD_CCtx* cctx = GetThreadLocalCCtx();
    size_t comp_size = ZSTD_compress_usingCDict(
        cctx,
        &(*output)[0],
        max_dst_size,
        input.data(),
        input.size(),
        dict->GetCDict()
    );

    if (ZSTD_isError(comp_size)) {
        return Status::Corruption("ZSTD dict compression failed: " +
                                  std::string(ZSTD_getErrorName(comp_size)));
    }

    output->resize(comp_size);
    return Status::OK();
}

Status ZSTDCodec::UncompressWithDict(
    const Slice& input,
    char* uncompressed_buf,
    size_t uncompressed_len,
    const ZSTDDictionary* dict
) {
    if (uncompressed_len == 0) return Status::OK();
    if (!uncompressed_buf) return Status::InvalidArgument("Null destination buffer");
    if (!dict || !dict->GetDDict()) {
        return Uncompress(input, uncompressed_buf, uncompressed_len);
    }

    ZSTD_DCtx* dctx = GetThreadLocalDCtx();
    size_t res = ZSTD_decompress_usingDDict(
        dctx,
        uncompressed_buf,
        uncompressed_len,
        input.data(),
        input.size(),
        dict->GetDDict()
    );

    if (ZSTD_isError(res) || res != uncompressed_len) {
        return Status::Corruption("ZSTD dict decompression failed or payload corrupt");
    }

    return Status::OK();
}

} // namespace lsm
