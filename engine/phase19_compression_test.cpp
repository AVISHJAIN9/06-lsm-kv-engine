#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

#include "compression_codec.hpp"
#include "snappy_codec.hpp"
#include "lz4_codec.hpp"
#include "zstd_codec.hpp"
#include "compression_policy.hpp"
#include "hardware_accel.hpp"

using namespace lsm;

// Helper to generate realistic repetitive + random test payloads
static std::string GenerateTestData(size_t size) {
    std::string data;
    data.reserve(size);
    const char* words[] = {"user_account", "balance_usd", "timestamp_epoch", "transaction_payload", "active_session_token"};
    size_t w_idx = 0;
    while (data.size() < size) {
        data.append(words[w_idx % 5]);
        data.append("=");
        data.append(std::to_string(100000 + (w_idx * 17) % 999999));
        data.append("; ");
        w_idx++;
    }
    data.resize(size);
    return data;
}

// -----------------------------------------------------------------------------
// Test 1: Codec Roundtrip Lossless Fidelity (Features 109 & 110)
// -----------------------------------------------------------------------------
static void TestCodecRoundtripLossless() {
    std::cout << "\n=== Test 1: Codec Roundtrip Lossless Fidelity (Features 109 & 110) ===\n";

    std::string original = GenerateTestData(64 * 1024); // 64KB block

    auto& registry = CodecRegistry::Instance();
    std::vector<CompressionType> codecs_to_test = {
        CompressionType::kNoCompression,
        CompressionType::kSnappyCompression,
        CompressionType::kLZ4Compression,
        CompressionType::kZSTD
    };

    for (auto type : codecs_to_test) {
        auto codec = registry.GetCodec(type);
        assert(codec != nullptr);

        std::string compressed;
        Status s = codec->Compress(Slice(original), &compressed);
        assert(s.ok());

        std::string decompressed;
        s = codec->Uncompress(Slice(compressed), &decompressed, original.size());
        assert(s.ok());
        assert(decompressed == original);

        double ratio = static_cast<double>(compressed.size()) / static_cast<double>(original.size()) * 100.0;
        std::cout << "  - Codec: " << std::setw(20) << std::left << codec->Name()
                  << " -> Orig: " << original.size() << " B, Comp: " << compressed.size()
                  << " B (" << std::fixed << std::setprecision(1) << ratio << "%), Fidelity: 100% MATCH\n";
    }

    std::cout << "  [PASSED] Test 1: Codec Roundtrip Lossless Fidelity\n";
}

// -----------------------------------------------------------------------------
// Test 2: Zstandard Trained Dictionary Efficiency (Feature 111)
// -----------------------------------------------------------------------------
static void TestZSTDTrainedDictionaryEfficiency() {
    std::cout << "\n=== Test 2: Zstandard Trained Dictionary Efficiency (Feature 111) ===\n";

    // Generate 1,000 JSON records (~120 bytes each)
    std::vector<std::string> records;
    records.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        std::ostringstream oss;
        oss << "{\"table\":\"customer_orders\",\"customer_id\":\"cust_" << std::setw(6) << std::setfill('0') << i
            << "\",\"status\":\"COMPLETED\",\"amount_cents\":" << (1000 + (i * 37) % 50000)
            << ",\"currency\":\"USD\",\"region\":\"us-east-1\"}";
        records.push_back(oss.str());
    }

    // Split into training samples (500) and test records (500)
    std::vector<std::string> training_samples(records.begin(), records.begin() + 500);
    std::vector<std::string> test_records(records.begin() + 500, records.end());

    ZSTDDictionary dict;
    Status s = dict.Train(training_samples, 16 * 1024);
    assert(s.ok());
    std::cout << "  - Trained ZSTD Dictionary: Size=" << dict.GetDictBuffer().size()
              << " B, DictID=" << dict.GetDictID() << "\n";

    ZSTDCodec zstd_codec;

    size_t total_raw_bytes = 0;
    size_t total_nodict_comp_bytes = 0;
    size_t total_dict_comp_bytes = 0;

    for (const auto& rec : test_records) {
        total_raw_bytes += rec.size();

        // Compress without dictionary
        std::string comp_nodict;
        s = zstd_codec.Compress(Slice(rec), &comp_nodict);
        assert(s.ok());
        total_nodict_comp_bytes += comp_nodict.size();

        // Compress with trained dictionary
        std::string comp_dict;
        s = zstd_codec.CompressWithDict(Slice(rec), &comp_dict, &dict);
        assert(s.ok());
        total_dict_comp_bytes += comp_dict.size();

        // Verify lossless decompression with dictionary
        std::string decomp_dict;
        decomp_dict.resize(rec.size());
        s = zstd_codec.UncompressWithDict(Slice(comp_dict), &decomp_dict[0], rec.size(), &dict);
        assert(s.ok());
        assert(decomp_dict == rec);
    }

    double nodict_ratio = static_cast<double>(total_nodict_comp_bytes) / static_cast<double>(total_raw_bytes) * 100.0;
    double dict_ratio = static_cast<double>(total_dict_comp_bytes) / static_cast<double>(total_raw_bytes) * 100.0;
    double savings_gain = (1.0 - (static_cast<double>(total_dict_comp_bytes) / static_cast<double>(total_nodict_comp_bytes))) * 100.0;

    std::cout << "  - Raw Records Total:        " << total_raw_bytes << " B (Avg: " << total_raw_bytes / 500 << " B)\n";
    std::cout << "  - ZSTD No-Dict Compressed:  " << total_nodict_comp_bytes << " B (" << nodict_ratio << "%)\n";
    std::cout << "  - ZSTD With-Dict Compressed:" << total_dict_comp_bytes << " B (" << dict_ratio << "%)\n";
    std::cout << "  - Dictionary Space Savings Gain: " << std::fixed << std::setprecision(2) << savings_gain << "%\n";

    assert(savings_gain >= 30.0); // >= 30% savings gain over non-dictionary compression
    std::cout << "  [PASSED] Test 2: Zstandard Trained Dictionary Efficiency\n";
}

// -----------------------------------------------------------------------------
// Test 3: LZ4 Ultra-High-Speed Decompression (Feature 112)
// -----------------------------------------------------------------------------
static void TestLZ4DecompressionThroughput() {
    std::cout << "\n=== Test 3: LZ4 Ultra-High-Speed Decompression (Feature 112) ===\n";

    LZ4Codec lz4;
    const size_t kBlockSize = 64 * 1024; // 64KB blocks
    const size_t kTotalBlocks = 1600;     // ~100MB total data
    std::string uncompressed_block = GenerateTestData(kBlockSize);

    std::string compressed_block;
    Status s = lz4.Compress(Slice(uncompressed_block), &compressed_block);
    assert(s.ok());

    std::vector<std::string> compressed_blocks(kTotalBlocks, compressed_block);
    std::string output_buf;
    output_buf.resize(kBlockSize);

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < kTotalBlocks; ++i) {
        s = lz4.Uncompress(Slice(compressed_blocks[i]), &output_buf[0], kBlockSize);
        assert(s.ok());
    }
    auto end = std::chrono::high_resolution_clock::now();

    double duration_sec = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1e9;
    double total_mb = (kTotalBlocks * kBlockSize) / (1024.0 * 1024.0);
    double throughput_gb_s = (total_mb / 1024.0) / duration_sec;

    std::cout << "  - Decompressed " << total_mb << " MB in " << (duration_sec * 1000.0)
              << " ms -> Throughput: " << std::fixed << std::setprecision(2) << throughput_gb_s << " GB/sec\n";

    assert(throughput_gb_s >= 1.5); // Assert >= 1.5 GB/sec
    std::cout << "  [PASSED] Test 3: LZ4 Ultra-High-Speed Decompression\n";
}

// -----------------------------------------------------------------------------
// Test 4: Dynamic Multi-Tier Level Compression Policies (Feature 113)
// -----------------------------------------------------------------------------
static void TestMultiTierCompressionPolicies() {
    std::cout << "\n=== Test 4: Dynamic Multi-Tier Level Compression Policies (Feature 113) ===\n";

    CompressionPolicyOptions opts;
    opts.per_level_compression = {
        CompressionType::kSnappyCompression, // L0: Fast write latency
        CompressionType::kSnappyCompression, // L1: Fast write latency
        CompressionType::kLZ4Compression,    // L2: High-throughput compaction
        CompressionType::kZSTD,              // L3: High ratio
        CompressionType::kZSTD,              // L4: High ratio
        CompressionType::kZSTD,              // L5: High ratio
        CompressionType::kZSTD               // L6: Max density
    };
    opts.min_compressible_block_size = 128;

    CompressionPolicyManager manager(opts);
    std::cout << "  - " << manager.DebugSummary() << "\n";

    // Test Level 0 evaluation
    assert(manager.EvaluateCompression(0, 4096) == CompressionType::kSnappyCompression);
    assert(manager.EvaluateCompression(1, 4096) == CompressionType::kSnappyCompression);
    assert(manager.EvaluateCompression(2, 4096) == CompressionType::kLZ4Compression);
    assert(manager.EvaluateCompression(3, 4096) == CompressionType::kZSTD);
    assert(manager.EvaluateCompression(6, 4096) == CompressionType::kZSTD);

    // Test sub-128B small block bypass
    assert(manager.EvaluateCompression(3, 64) == CompressionType::kNoCompression);

    // Dynamic runtime update
    manager.SetLevelCompression(0, CompressionType::kNoCompression);
    assert(manager.EvaluateCompression(0, 4096) == CompressionType::kNoCompression);
    std::cout << "  - Dynamically updated Level 0 to kNoCompression for zero-CPU foreground flushes.\n";

    std::cout << "  [PASSED] Test 4: Dynamic Multi-Tier Level Compression Policies\n";
}

// -----------------------------------------------------------------------------
// Test 5: Corrupted Block Decompression Interception (Features 109–112)
// -----------------------------------------------------------------------------
static void TestCorruptedBlockInterception() {
    std::cout << "\n=== Test 5: Corrupted Block Decompression Interception (Features 109-112) ===\n";

    std::string original = GenerateTestData(4096);
    std::string decomp_buf;
    decomp_buf.resize(original.size());

    // 1. Snappy Corruption
    {
        SnappyCodec snappy;
        std::string comp;
        assert(snappy.Compress(Slice(original), &comp).ok());

        // Truncate payload
        std::string truncated = comp.substr(0, comp.size() / 2);
        Status s = snappy.Uncompress(Slice(truncated), &decomp_buf[0], original.size());
        assert(s.IsCorruption());

        // Corrupt length varint header (high bit set without continuation)
        std::string corrupt_header = comp;
        corrupt_header[0] = static_cast<char>(0x80);
        corrupt_header.resize(1);
        s = snappy.Uncompress(Slice(corrupt_header), &decomp_buf[0], original.size());
        assert(s.IsCorruption());

        // Corrupt tag to invalid copy offset (offset 0 is illegal in Snappy)
        std::string corrupt_tag = comp;
        if (corrupt_tag.size() > 2) {
            corrupt_tag[1] = static_cast<char>(0x01); // 1-byte copy tag with 0 offset
            corrupt_tag[2] = static_cast<char>(0x00);
            s = snappy.Uncompress(Slice(corrupt_tag), &decomp_buf[0], original.size());
            assert(s.IsCorruption());
        }
        std::cout << "  - Snappy safely intercepted corrupted / truncated block: " << s.ToString() << "\n";
    }

    // 2. LZ4 Corruption
    {
        LZ4Codec lz4;
        std::string comp;
        assert(lz4.Compress(Slice(original), &comp).ok());

        std::string corrupted = comp;
        corrupted[corrupted.size() / 2] ^= 0x5a;
        Status s = lz4.Uncompress(Slice(corrupted), &decomp_buf[0], original.size());
        assert(s.IsCorruption());
        std::cout << "  - LZ4 safely intercepted corrupted block: " << s.ToString() << "\n";
    }

    // 3. ZSTD Corruption
    {
        ZSTDCodec zstd;
        std::string comp;
        assert(zstd.Compress(Slice(original), &comp).ok());

        std::string corrupted = comp;
        corrupted[10] ^= 0x7f;
        Status s = zstd.Uncompress(Slice(corrupted), &decomp_buf[0], original.size());
        assert(s.IsCorruption());
        std::cout << "  - ZSTD safely intercepted corrupted block: " << s.ToString() << "\n";
    }

    std::cout << "  [PASSED] Test 5: Corrupted Block Decompression Interception\n";
}

// -----------------------------------------------------------------------------
// Test 6: SIMD Hardware CRC32C & Bit-Packing (Feature 114)
// -----------------------------------------------------------------------------
static void TestSIMDHardwareCRCAndBitPacking() {
    std::cout << "\n=== Test 6: SIMD Hardware CRC32C & Bit-Packing (Feature 114) ===\n";

    std::cout << "  - " << HardwareFeatures::FeatureString() << "\n";

    // 1. Hardware vs Scalar CRC32C Verification
    const size_t kBufferSize = 1024 * 1024; // 1MB buffer
    std::string test_buffer = GenerateTestData(kBufferSize);

    uint32_t scalar_crc = SIMDChecksum::ScalarCRC32C(test_buffer.data(), test_buffer.size());
    uint32_t simd_crc = SIMDChecksum::CRC32C(test_buffer.data(), test_buffer.size());

    assert(scalar_crc == simd_crc);
    std::cout << "  - Scalar CRC32C = 0x" << std::hex << scalar_crc
              << ", SIMD CRC32C = 0x" << simd_crc << std::dec << " (100% Mathematical Match)\n";

    // Benchmark SIMD CRC32C Throughput
    const size_t kIterations = 1000; // 1GB total data
    auto start_simd = std::chrono::high_resolution_clock::now();
    uint32_t sink = 0;
    for (size_t i = 0; i < kIterations; ++i) {
        sink += SIMDChecksum::CRC32C(test_buffer.data(), test_buffer.size());
    }
    auto end_simd = std::chrono::high_resolution_clock::now();
    (void)sink;

    double simd_duration_sec = std::chrono::duration_cast<std::chrono::nanoseconds>(end_simd - start_simd).count() / 1e9;
    double simd_gb_s = 1.0 / simd_duration_sec;

    std::cout << "  - SIMD Hardware CRC32C Throughput: " << std::fixed << std::setprecision(2)
              << simd_gb_s << " GB/sec\n";
    assert(simd_gb_s >= 4.0); // Assert high throughput hardware CRC32C

    // 2. SIMD Vectorized Delta Bit-Packing on 10,000 Restart Points
    const size_t kCount = 10000;
    std::vector<uint32_t> restart_offsets(kCount);
    for (size_t i = 0; i < kCount; ++i) {
        restart_offsets[i] = 100000 + static_cast<uint32_t>(i * 48); // Delta increases by 48
    }

    uint32_t base_val = restart_offsets[0];
    uint32_t max_delta = restart_offsets.back() - base_val;
    uint8_t bits = SIMDBitPacking::BitsNeeded(max_delta);

    std::vector<uint8_t> packed_buffer((kCount * bits + 7) / 8);
    size_t packed_bytes = SIMDBitPacking::PackDeltas32(
        restart_offsets.data(),
        kCount,
        base_val,
        bits,
        packed_buffer.data()
    );

    std::vector<uint32_t> unpacked_offsets(kCount, 0);
    SIMDBitPacking::UnpackDeltas32(
        packed_buffer.data(),
        kCount,
        base_val,
        bits,
        unpacked_offsets.data()
    );

    assert(unpacked_offsets == restart_offsets);

    double compression_ratio = static_cast<double>(packed_bytes) / (kCount * sizeof(uint32_t)) * 100.0;
    std::cout << "  - Vectorized Delta Bit-Packing (10,000 restart offsets): "
              << (kCount * sizeof(uint32_t)) << " B -> " << packed_bytes
              << " B (" << std::fixed << std::setprecision(1) << compression_ratio << "% size, "
              << static_cast<int>(bits) << " bits/int, 100% Fidelity OK)\n";

    std::cout << "  [PASSED] Test 6: SIMD Hardware CRC32C & Bit-Packing\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase XIX Verification Test Suite\n";
    std::cout << "  Features 109-114: Storage Engine Compression & Block Encodings (Part 1)\n";
    std::cout << "=======================================================================\n";

    TestCodecRoundtripLossless();
    TestZSTDTrainedDictionaryEfficiency();
    TestLZ4DecompressionThroughput();
    TestMultiTierCompressionPolicies();
    TestCorruptedBlockInterception();
    TestSIMDHardwareCRCAndBitPacking();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase XIX Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
