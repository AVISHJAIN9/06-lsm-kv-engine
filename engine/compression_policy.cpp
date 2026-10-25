#include "compression_policy.hpp"
#include <sstream>

namespace lsm {

CompressionPolicyManager::CompressionPolicyManager(const CompressionPolicyOptions& options)
    : options_(options) {
    if (options_.per_level_compression.size() < Version::kNumLevels) {
        options_.per_level_compression.resize(
            Version::kNumLevels,
            options_.default_compression
        );
    }
}

void CompressionPolicyManager::SetLevelCompression(int level, CompressionType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level >= 0 && level < Version::kNumLevels) {
        if (options_.per_level_compression.size() <= static_cast<size_t>(level)) {
            options_.per_level_compression.resize(Version::kNumLevels, options_.default_compression);
        }
        options_.per_level_compression[level] = type;
    }
}

CompressionType CompressionPolicyManager::GetLevelCompression(int level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level >= 0 && static_cast<size_t>(level) < options_.per_level_compression.size()) {
        return options_.per_level_compression[level];
    }
    return options_.default_compression;
}

CompressionType CompressionPolicyManager::EvaluateCompression(int level, size_t block_size) const {
    if (block_size < options_.min_compressible_block_size) {
        return CompressionType::kNoCompression;
    }
    return GetLevelCompression(level);
}

std::string CompressionPolicyManager::DebugSummary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << "CompressionPolicy [Default: " << static_cast<int>(options_.default_compression) << "] Levels: ";
    for (size_t l = 0; l < options_.per_level_compression.size(); ++l) {
        oss << "L" << l << ":" << static_cast<int>(options_.per_level_compression[l]) << " ";
    }
    return oss.str();
}

} // namespace lsm
