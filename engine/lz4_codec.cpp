#include "lz4_codec.hpp"
#include <lz4.h>

namespace lsm {

size_t LZ4Codec::MaxCompressedLength(size_t input_len) const {
    if (input_len > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
        return 0;
    }
    return static_cast<size_t>(LZ4_compressBound(static_cast<int>(input_len)));
}

Status LZ4Codec::Compress(
    const Slice& input,
    std::string* output,
    const CompressionOptions& opts
) {
    (void)opts;
    if (!output) return Status::InvalidArgument("Null output string buffer");

    if (input.size() == 0) {
        output->clear();
        return Status::OK();
    }

    if (input.size() > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
        return Status::InvalidArgument("LZ4 input exceeds max block limit");
    }

    int max_dst_size = LZ4_compressBound(static_cast<int>(input.size()));
    output->resize(max_dst_size);

    int comp_size = LZ4_compress_default(
        input.data(),
        &(*output)[0],
        static_cast<int>(input.size()),
        max_dst_size
    );

    if (comp_size <= 0) {
        return Status::Corruption("LZ4 compression failed");
    }

    output->resize(comp_size);
    return Status::OK();
}

Status LZ4Codec::Uncompress(
    const Slice& input,
    char* uncompressed_buf,
    size_t uncompressed_len
) {
    if (uncompressed_len == 0) return Status::OK();
    if (!uncompressed_buf) return Status::InvalidArgument("Null destination buffer");

    int res = LZ4_decompress_safe(
        input.data(),
        uncompressed_buf,
        static_cast<int>(input.size()),
        static_cast<int>(uncompressed_len)
    );

    if (res < 0 || static_cast<size_t>(res) != uncompressed_len) {
        return Status::Corruption("LZ4 decompression failed or payload corrupt");
    }

    return Status::OK();
}

} // namespace lsm
