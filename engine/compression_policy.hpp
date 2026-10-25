#pragma once

#include <vector>
#include <string>
#include <mutex>
#include "compression_codec.hpp"
#include "version.hpp"

namespace lsm {

// CompressionPolicyOptions: Configures multi-tier compression strategies across LSM levels (Feature 113).
struct CompressionPolicyOptions {
    CompressionType default_compression{CompressionType::kSnappyCompression};
    std::vector<CompressionType> per_level_compression{
        CompressionType::kSnappyCompression, // Level 0 (Fast write latency)
        CompressionType::kSnappyCompression, // Level 1 (Fast write latency)
        CompressionType::kZSTD,              // Level 2 (High compression ratio)
        CompressionType::kZSTD,              // Level 3
        CompressionType::kZSTD,              // Level 4
        CompressionType::kZSTD,              // Level 5
        CompressionType::kZSTD               // Level 6
    };
    size_t min_compressible_block_size{128}; // Bypass compression if block < 128 bytes
};

// CompressionPolicyManager: Evaluates and enforces level-specific compression rules (Feature 113).
class CompressionPolicyManager {
public:
    explicit CompressionPolicyManager(
        const CompressionPolicyOptions& options = CompressionPolicyOptions{}
    );
    ~CompressionPolicyManager() = default;

    void SetLevelCompression(int level, CompressionType type);
    CompressionType GetLevelCompression(int level) const;

    // Evaluates optimal compression type considering level and block payload size
    CompressionType EvaluateCompression(int level, size_t block_size) const;

    std::string DebugSummary() const;

private:
    mutable std::mutex mutex_{};
    CompressionPolicyOptions options_{};
};

} // namespace lsm
