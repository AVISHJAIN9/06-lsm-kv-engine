#include "compression_codec.hpp"
#include "snappy_codec.hpp"
#include "lz4_codec.hpp"
#include "zstd_codec.hpp"

namespace lsm {

CodecRegistry& CodecRegistry::Instance() {
    static CodecRegistry registry;
    return registry;
}

CodecRegistry::CodecRegistry() {
    // Register standard built-in compression codecs
    Register(CompressionType::kNoCompression, std::make_shared<NoCompressionCodec>());
    Register(CompressionType::kSnappyCompression, std::make_shared<SnappyCodec>());
    Register(CompressionType::kLZ4Compression, std::make_shared<LZ4Codec>());
    Register(CompressionType::kZSTD, std::make_shared<ZSTDCodec>());
}

void CodecRegistry::Register(CompressionType type, std::shared_ptr<CompressionCodec> codec) {
    std::lock_guard<std::mutex> lock(mutex_);
    codecs_[static_cast<uint8_t>(type)] = std::move(codec);
}

std::shared_ptr<CompressionCodec> CodecRegistry::GetCodec(CompressionType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = codecs_.find(static_cast<uint8_t>(type));
    if (it != codecs_.end()) {
        return it->second;
    }
    // Fallback to NoCompression
    auto no_comp = codecs_.find(static_cast<uint8_t>(CompressionType::kNoCompression));
    if (no_comp != codecs_.end()) {
        return no_comp->second;
    }
    return nullptr;
}

bool CodecRegistry::IsSupported(CompressionType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return codecs_.find(static_cast<uint8_t>(type)) != codecs_.end();
}

} // namespace lsm
