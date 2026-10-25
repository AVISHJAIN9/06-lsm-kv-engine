#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <random>
#include <cmath>

#include "index_builder.hpp"
#include "index_reader.hpp"
#include "bloom_filter.hpp"
#include "prefix_compression.hpp"
#include "block_trailer_envelope.hpp"
#include "table_properties.hpp"
#include "sstable_version.hpp"
#include "block_builder.hpp"
#include "block_reader.hpp"

using namespace lsm;

// -----------------------------------------------------------------------------
// Test 1: Sparse Index Binary Search & Block Isolation (Feature 43)
// -----------------------------------------------------------------------------
static void Test1_SparseIndexBinarySearchAndTwoLevel() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 1: Sparse Index Binary Search & Block Isolation\n";
    std::cout << "=================================================================\n";

    IndexBuilder index_builder(1);
    const int kNumBlocks = 20;
    const int kKeysPerBlock = 50;

    std::vector<BlockHandle> true_handles;
    std::vector<std::string> block_first_keys;
    std::vector<std::string> block_last_keys;

    uint64_t current_file_offset = 0;

    for (int b = 0; b < kNumBlocks; ++b) {
        char first_key[32], last_key[32];
        std::snprintf(first_key, sizeof(first_key), "key_%04d_00", b);
        std::snprintf(last_key, sizeof(last_key), "key_%04d_%02d", b, kKeysPerBlock - 1);

        block_first_keys.push_back(first_key);
        block_last_keys.push_back(last_key);

        uint64_t block_size = 4096 + b * 64;
        BlockHandle handle(current_file_offset, block_size);
        true_handles.push_back(handle);
        current_file_offset += block_size + BlockTrailerEnvelope::kTrailerSize;
    }

    // Add index entries
    for (int b = 0; b < kNumBlocks; ++b) {
        std::string last_k = block_last_keys[b];
        const Slice* next_first = (b + 1 < kNumBlocks) ? new Slice(block_first_keys[b + 1]) : nullptr;

        index_builder.AddIndexEntry(&last_k, next_first, true_handles[b]);
        delete next_first;
    }

    Slice index_block = index_builder.Finish();
    std::cout << "  - Built Single-Level Sparse Index: " << index_block.size()
              << " bytes for " << kNumBlocks << " data blocks\n";

    IndexReader index_reader(index_block);

    // Verify binary search locates exact block handle for every key
    const int kLookupTrials = 2000;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kLookupTrials; ++i) {
        int b = i % kNumBlocks;
        int k_idx = (i * 7) % kKeysPerBlock;
        char query_key[32];
        std::snprintf(query_key, sizeof(query_key), "key_%04d_%02d", b, k_idx);

        BlockHandle found_handle;
        bool found = index_reader.FindBlockHandle(Slice(query_key), &found_handle);
        assert(found);
        assert(found_handle.offset() == true_handles[b].offset());
        assert(found_handle.size() == true_handles[b].size());
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double total_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    double avg_lookup_us = total_us / kLookupTrials;

    std::cout << "  - Executed " << kLookupTrials << " index binary search lookups\n";
    std::cout << "  - Average Binary Search Latency: " << avg_lookup_us << " us (Target <= 2.0 us)\n";
    assert(avg_lookup_us <= 2.0);

    // Test Two-Level Index Partitioning
    TwoLevelIndexBuilder two_level_builder(256, 1); // 256B partition cap
    std::vector<std::string> partition_blocks;

    for (int b = 0; b < kNumBlocks; ++b) {
        std::string last_k = block_last_keys[b];
        const Slice* next_first = (b + 1 < kNumBlocks) ? new Slice(block_first_keys[b + 1]) : nullptr;
        two_level_builder.AddIndexEntry(&last_k, next_first, true_handles[b], &partition_blocks);
        delete next_first;
    }
    Slice top_level_index = two_level_builder.Finish(&partition_blocks);

    std::cout << "  - Built Two-Level Partitioned Index: " << partition_blocks.size()
              << " partitions, Top Index Size: " << top_level_index.size() << " bytes\n";
    assert(two_level_builder.IsTwoLevel());
    assert(!partition_blocks.empty());

    std::cout << "[PASSED] Test 1: Sparse Index & Two-Level Partitioning Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 2: Block-Based Bloom Filter Engine & FPR Conformance (Feature 44)
// -----------------------------------------------------------------------------
static void Test2_BlockBasedBloomFilterEngine() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 2: Block-Based Bloom Filter & FPR Conformance\n";
    std::cout << "=================================================================\n";

    BloomFilterPolicy policy(10); // 10 bits per key
    const int kNumKeys = 10000;

    std::vector<std::string> key_strings;
    std::vector<Slice> key_slices;
    key_strings.reserve(kNumKeys);
    key_slices.reserve(kNumKeys);

    for (int i = 0; i < kNumKeys; ++i) {
        key_strings.push_back("user_account_id_" + std::to_string(i * 3 + 1));
        key_slices.push_back(Slice(key_strings.back()));
    }

    std::string filter_data;
    policy.CreateFilter(key_slices.data(), kNumKeys, &filter_data);

    std::cout << "  - Generated Bloom Filter: " << filter_data.size() << " bytes for "
              << kNumKeys << " keys (" << (filter_data.size() * 8.0 / kNumKeys) << " bits/key)\n";

    // 1. Zero false negatives check
    for (int i = 0; i < kNumKeys; ++i) {
        bool match = policy.KeyMayMatch(key_slices[i], Slice(filter_data));
        assert(match);
    }
    std::cout << "  - Verified Zero False Negatives across " << kNumKeys << " inserted keys (100% Match)\n";

    // 2. False positive rate measurement over 10,000 non-existent keys
    int false_positives = 0;
    const int kNegativeQueries = 10000;
    std::vector<std::string> neg_strings(kNegativeQueries);
    std::vector<Slice> neg_keys(kNegativeQueries);
    for (int i = 0; i < kNegativeQueries; ++i) {
        neg_strings[i] = "user_account_id_" + std::to_string(i * 3 + 2); // Disjoint keys
        neg_keys[i] = Slice(neg_strings[i]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kNegativeQueries; ++i) {
        if (policy.KeyMayMatch(neg_keys[i], Slice(filter_data))) {
            false_positives++;
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double total_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    double avg_eval_ns = total_ns / kNegativeQueries;
    double fpr = (static_cast<double>(false_positives) / kNegativeQueries) * 100.0;

    std::cout << "  - False Positives: " << false_positives << " / " << kNegativeQueries
              << " (" << fpr << "% FPR, Target <= 1.0%)\n";
    std::cout << "  - Average Probe Latency: " << avg_eval_ns << " ns (Target <= 100 ns)\n";

    assert(fpr <= 1.0);
    assert(avg_eval_ns <= 100.0);

    // 3. Test Block-Based Localized FilterBlockBuilder & Reader
    FilterBlockBuilder fbuilder(&policy, 2048);
    for (int b = 0; b < 5; ++b) {
        fbuilder.StartBlock(b * 2048);
        for (int k = 0; k < 100; ++k) {
            std::string k_str = "blk_" + std::to_string(b) + "_key_" + std::to_string(k);
            fbuilder.AddKey(Slice(k_str));
        }
    }
    Slice filter_block = fbuilder.Finish();
    FilterBlockReader freader(&policy, filter_block);

    for (int b = 0; b < 5; ++b) {
        for (int k = 0; k < 100; ++k) {
            std::string k_str = "blk_" + std::to_string(b) + "_key_" + std::to_string(k);
            bool match = freader.KeyMayMatch(b * 2048, Slice(k_str));
            assert(match);
        }
    }

    std::cout << "[PASSED] Test 2: Optimal Bloom Filter Engine & Localized FilterBlock Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 3: Intra-Block Key Prefix Compression Space Savings (Feature 45)
// -----------------------------------------------------------------------------
static void Test3_IntraBlockPrefixCompression() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 3: Intra-Block Key Prefix Compression Engine\n";
    std::cout << "=================================================================\n";

    const int kRestartInterval = 16;
    BlockBuilder builder(kRestartInterval);

    const int kNumKeys = 1000;
    size_t uncompressed_key_bytes = 0;

    for (int i = 0; i < kNumKeys; ++i) {
        char key_buf[128];
        std::snprintf(key_buf, sizeof(key_buf), "us-west-2:production:service_cluster_omega_01:host_node_%05d:telemetry:metrics", i);
        std::string val = "v_" + std::to_string(i);

        uncompressed_key_bytes += std::strlen(key_buf);
        builder.Add(Slice(key_buf), Slice(val));
    }

    Slice compressed_block = builder.Finish();

    double compression_ratio = static_cast<double>(uncompressed_key_bytes) / compressed_block.size();
    std::cout << "  - Uncompressed Key Bytes:  " << uncompressed_key_bytes << " bytes\n";
    std::cout << "  - Compressed Block Size:   " << compressed_block.size() << " bytes\n";
    std::cout << "  - Key Compression Ratio:   " << compression_ratio << "x (Target >= 2.5x)\n";

    assert(compression_ratio >= 2.5);

    // Random seek latency test within block
    BlockReader reader(compressed_block);
    auto iter = reader.NewIterator();

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 500; ++i) {
        int idx = (i * 17) % kNumKeys;
        char target_key[128];
        std::snprintf(target_key, sizeof(target_key), "us-west-2:production:service_cluster_omega_01:host_node_%05d:telemetry:metrics", idx);

        iter->Seek(Slice(target_key));
        assert(iter->Valid());
        assert(iter->key() == Slice(target_key));
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double total_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    double avg_seek_us = total_us / 500.0;

    std::cout << "  - Average Intra-Block Seek Latency: " << avg_seek_us << " us (Target <= 3.0 us)\n";
    assert(avg_seek_us <= 3.0);

    std::cout << "[PASSED] Test 3: Key Prefix Delta Compression Ratio >= 2.5x Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 4: Fixed 5-Byte Block Trailer Framing & Checksum Envelope (Feature 46)
// -----------------------------------------------------------------------------
static void Test4_FixedBlockTrailerFraming() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 4: Fixed 5-Byte Block Trailer Framing & CRC32C\n";
    std::cout << "=================================================================\n";

    std::string payload = "Arbitrary physical SSTable block buffer data...";
    std::string block_buf = payload;

    BlockTrailerEnvelope::PackTrailer(&block_buf, BlockTrailerEnvelope::CompressionType::kSnappyCompression);
    assert(block_buf.size() == payload.size() + BlockTrailerEnvelope::kTrailerSize);

    // 1. Clean verification
    Slice stripped_payload;
    BlockTrailerEnvelope::CompressionType comp_type;
    Status s = BlockTrailerEnvelope::VerifyAndStripTrailer(Slice(block_buf), &stripped_payload, &comp_type);
    assert(s.ok());
    assert(stripped_payload == Slice(payload));
    assert(comp_type == BlockTrailerEnvelope::CompressionType::kSnappyCompression);
    std::cout << "  - Clean Trailer Verification: OK (Type = kSnappyCompression)\n";

    // 2. Bit-flip in block payload
    std::string corrupt_payload_block = block_buf;
    corrupt_payload_block[5] ^= 0x20;

    Status bad_s1 = BlockTrailerEnvelope::VerifyAndStripTrailer(Slice(corrupt_payload_block), &stripped_payload);
    assert(!bad_s1.ok());
    assert(bad_s1.IsCorruption());
    std::cout << "  - Payload Bit-Flip Interception: " << bad_s1.ToString() << " (PASSED)\n";

    // 3. Bit-flip in CRC trailer
    std::string corrupt_crc_block = block_buf;
    corrupt_crc_block[corrupt_crc_block.size() - 2] ^= 0x04;

    Status bad_s2 = BlockTrailerEnvelope::VerifyAndStripTrailer(Slice(corrupt_crc_block), &stripped_payload);
    assert(!bad_s2.ok());
    assert(bad_s2.IsCorruption());
    std::cout << "  - Trailer Checksum Tamper Interception: " << bad_s2.ToString() << " (PASSED)\n";

    std::cout << "[PASSED] Test 4: Fixed 5-Byte Block Trailer Envelope Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 5: SSTable File Metadata & Key-Range Statistics Block (Feature 47)
// -----------------------------------------------------------------------------
static void Test5_TablePropertiesMetadataBlock() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 5: Table Properties & Key-Range Statistics\n";
    std::cout << "=================================================================\n";

    TableProperties props;
    props.num_entries = 50000;
    props.num_data_blocks = 250;
    props.data_size = 1048576;
    props.index_size = 16384;
    props.filter_size = 65536;
    props.raw_key_size = 800000;
    props.raw_value_size = 1200000;
    props.smallest_key = "customer_00001";
    props.largest_key = "customer_50000";
    props.min_seq = 100;
    props.max_seq = 50100;
    props.format_version = 2;
    props.feature_flags = TableFeatureFlags::kFlagBlockBloom | TableFeatureFlags::kFlagTwoLevelIndex;
    props.column_family_name = "account_cf";

    std::string serialized_block;
    props.EncodeTo(&serialized_block);
    std::cout << "  - Serialized TableProperties Block: " << serialized_block.size() << " bytes\n";

    TableProperties decoded_props;
    Status s = decoded_props.DecodeFrom(Slice(serialized_block));
    assert(s.ok());

    assert(decoded_props.num_entries == 50000);
    assert(decoded_props.num_data_blocks == 250);
    assert(decoded_props.data_size == 1048576);
    assert(decoded_props.index_size == 16384);
    assert(decoded_props.filter_size == 65536);
    assert(decoded_props.smallest_key == "customer_00001");
    assert(decoded_props.largest_key == "customer_50000");
    assert(decoded_props.min_seq == 100);
    assert(decoded_props.max_seq == 50100);
    assert(decoded_props.column_family_name == "account_cf");
    assert(decoded_props.feature_flags == props.feature_flags);

    // Test O(1) Key Range Pruning
    assert(decoded_props.KeyInRange(Slice("customer_25000")));
    assert(!decoded_props.KeyInRange(Slice("customer_00000")));
    assert(!decoded_props.KeyInRange(Slice("customer_99999")));

    assert(decoded_props.Overlaps(Slice("customer_10000"), Slice("customer_20000")));
    assert(!decoded_props.Overlaps(Slice("aaa"), Slice("customer_00000")));

    std::cout << decoded_props.ToString();
    std::cout << "[PASSED] Test 5: Table Properties Serialization & Range Pruning 100% Verified.\n";
}

// -----------------------------------------------------------------------------
// Test 6: SSTable Format Versioning & Feature Flag Compatibility (Feature 48)
// -----------------------------------------------------------------------------
static void Test6_TableVersioningAndFeatureFlags() {
    std::cout << "\n=================================================================\n";
    std::cout << ">>> RUNNING TEST 6: Table Versioning & Feature Flags Bitset\n";
    std::cout << "=================================================================\n";

    BlockHandle meta_handle(1024, 256);
    BlockHandle index_handle(2048, 512);

    // 1. Valid V2 VersionedFooter
    VersionedFooter v2_footer(
        meta_handle,
        index_handle,
        static_cast<uint32_t>(TableFormatVersion::kFormatVersionV2),
        TableFeatureFlags::kFlagTwoLevelIndex | TableFeatureFlags::kFlagBlockBloom
    );

    std::string footer_buf;
    v2_footer.EncodeTo(&footer_buf);
    assert(footer_buf.size() == 48);

    VersionedFooter decoded_v2;
    Slice input(footer_buf);
    Status s = decoded_v2.DecodeFrom(&input);
    assert(s.ok());
    assert(decoded_v2.version() == static_cast<uint32_t>(TableFormatVersion::kFormatVersionV2));
    assert(decoded_v2.flags() == (TableFeatureFlags::kFlagTwoLevelIndex | TableFeatureFlags::kFlagBlockBloom));
    std::cout << "  - Valid Version v2 & Feature Flags verified: OK\n";

    // 2. Reject unsupported future format version (v99)
    VersionedFooter future_footer(meta_handle, index_handle, 99, TableFeatureFlags::kFlagNone);
    std::string future_buf;
    future_footer.EncodeTo(&future_buf);

    Slice future_input(future_buf);
    VersionedFooter decoded_future;
    Status bad_ver_s = decoded_future.DecodeFrom(&future_input);
    assert(!bad_ver_s.ok());
    assert(bad_ver_s.IsNotSupported());
    std::cout << "  - Unsupported future version rejection: " << bad_ver_s.ToString() << " (PASSED)\n";

    // 3. Reject unsupported future capability flag bit (e.g. 0x80000000)
    VersionedFooter bad_flag_footer(
        meta_handle,
        index_handle,
        static_cast<uint32_t>(TableFormatVersion::kCurrentVersion),
        0x80000000
    );
    std::string bad_flag_buf;
    bad_flag_footer.EncodeTo(&bad_flag_buf);

    Slice bad_flag_input(bad_flag_buf);
    VersionedFooter decoded_bad_flag;
    Status bad_flag_s = decoded_bad_flag.DecodeFrom(&bad_flag_input);
    assert(!bad_flag_s.ok());
    assert(bad_flag_s.IsNotSupported());
    std::cout << "  - Unsupported feature flag bit rejection: " << bad_flag_s.ToString() << " (PASSED)\n";

    std::cout << "[PASSED] Test 6: SSTable Format Versioning & Feature Flags Bitsets Verified.\n";
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << ">>> STARTING LSM-TREE PHASE VIII SSTABLE FORMAT VERIFICATION  <<<\n";
    std::cout << ">>> Features 43-48: Sparse Index, Bloom Filter, Prefix Delta,  <<<\n";
    std::cout << ">>> Trailer Framing, Table Properties & Versioned Bitsets    <<<\n";
    std::cout << "=================================================================\n";

    try {
        Test1_SparseIndexBinarySearchAndTwoLevel();
        Test2_BlockBasedBloomFilterEngine();
        Test3_IntraBlockPrefixCompression();
        Test4_FixedBlockTrailerFraming();
        Test5_TablePropertiesMetadataBlock();
        Test6_TableVersioningAndFeatureFlags();

        std::cout << "\n=================================================================\n";
        std::cout << ">>> [ALL 6 PHASE VIII VERIFICATION TESTS PASSED 100% ZERO ERRORS] <<<\n";
        std::cout << "=================================================================\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FATAL TEST EXCEPTION: " << e.what() << "\n";
        return 1;
    }
}
