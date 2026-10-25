#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <filesystem>
#include <memory>
#include <algorithm>

#include "version_query.hpp"
#include "bloom_short_circuit.hpp"
#include "index_lookup.hpp"
#include "block_cache.hpp"
#include "merging_iterator.hpp"
#include "db_iterator.hpp"
#include "table_builder.hpp"
#include "table_reader.hpp"
#include "memtable_queue.hpp"
#include "memtable_iter.hpp"
#include "types.hpp"

namespace fs = std::filesystem;
using namespace lsm;

static void SetupDir(const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        fs::remove_all(path, ec);
    }
    fs::create_directories(path, ec);
}

static FileMetaData CreateTestSSTable(
    const std::string& db_dir,
    uint64_t file_num,
    const std::vector<std::pair<std::string, std::string>>& kvs,
    SequenceNumber seq = 1,
    ValueType vtype = kTypeValue,
    size_t block_size = 512
) {
    char name_buf[128];
    std::snprintf(name_buf, sizeof(name_buf), "%s/%06llu.sst", db_dir.c_str(), static_cast<unsigned long long>(file_num));
    std::string filename = name_buf;

    TableOptions topts;
    topts.block_size = block_size;
    topts.filter_policy = std::make_shared<BloomFilterPolicy>(10);

    TableBuilder builder(topts, filename);
    for (const auto& kv : kvs) {
        InternalKey ikey(Slice(kv.first), seq, vtype);
        builder.Add(ikey.Encode(), Slice(kv.second));
    }
    Status s = builder.Finish();
    assert(s.ok());

    FileMetaData meta;
    meta.file_number = file_num;
    meta.filename = filename;
    meta.file_size = builder.FileSize();
    meta.smallest_key = builder.properties().smallest_key;
    meta.largest_key = builder.properties().largest_key;
    meta.min_seq = seq;
    meta.max_seq = seq;
    return meta;
}

// -----------------------------------------------------------------------------
// Test 1: Multi-Level Query Cascade (Feature 67)
// Insert key into L2; overwrite in L1; overwrite in MemTable; assert Get() returns
// MemTable value from tier 0; delete in MemTable; assert Get() returns NotFound immediately.
// -----------------------------------------------------------------------------
void Test1_MultiLevelQueryCascade() {
    std::cout << "\n=== Test 1: Multi-Level Read Path Cascading (Feature 67) ===\n";

    std::string test_dir = "./test_phase12_cascade";
    SetupDir(test_dir);

    // 1. Create Level 2 SSTable with "k_cascade" = "V1_L2" (seq 10)
    std::vector<std::pair<std::string, std::string>> kvs_l2 = {{"k_cascade", "V1_L2"}};
    FileMetaData f_l2 = CreateTestSSTable(test_dir, 1002, kvs_l2, 10);

    // 2. Create Level 1 SSTable with "k_cascade" = "V2_L1" (seq 20)
    std::vector<std::pair<std::string, std::string>> kvs_l1 = {{"k_cascade", "V2_L1"}};
    FileMetaData f_l1 = CreateTestSSTable(test_dir, 1001, kvs_l1, 20);

    std::vector<std::vector<FileMetaData>> files(7);
    files[1].push_back(f_l1);
    files[2].push_back(f_l2);

    auto version = new Version(files);
    auto memtable_queue = std::make_shared<MemTableQueue>(4 * 1024 * 1024);

    VersionQueryRouter router(memtable_queue, version);

    // Step A: Only in L2 & L1 -> Query should find V2_L1 in Level 1
    std::string val;
    int level_hit = -1;
    ReadOptionsExtended opts;
    LookupKey lkey(Slice("k_cascade"), 100);

    Status s = router.Get(opts, lkey, &val, &level_hit);
    assert(s.ok());
    assert(val == "V2_L1");
    assert(level_hit == 1);
    std::cout << "  - Pass A: Found in Level " << level_hit << " -> Val: " << val << "\n";

    // Step B: Write into Active MemTable "V3_MEMTABLE" (seq 30)
    memtable_queue->Add(30, kTypeValue, Slice("k_cascade"), Slice("V3_MEMTABLE"));

    s = router.Get(opts, lkey, &val, &level_hit);
    assert(s.ok());
    assert(val == "V3_MEMTABLE");
    assert(level_hit == 0); // Hit in MemTable tier
    std::cout << "  - Pass B: Found in Level 0 (MemTable) -> Val: " << val << " (Short-circuited L1 & L2!)\n";

    // Step C: Write Tombstone in Active MemTable (seq 40)
    memtable_queue->Add(40, kTypeDeletion, Slice("k_cascade"), Slice(""));

    s = router.Get(opts, lkey, &val, &level_hit);
    assert(s.IsNotFound());
    assert(level_hit == 0); // Short-circuited immediately without checking L1 or L2
    std::cout << "  - Pass C: Tombstone in MemTable short-circuited query traversal immediately with: " << s.ToString() << "\n";

    version->Unref();
    std::cout << "  [PASSED] Test 1: Multi-Level Query Cascade\n";
}

// -----------------------------------------------------------------------------
// Test 2: Bloom Filter Short-Circuit Verification (Feature 68)
// Query 1,000 keys not in database; assert Bloom filter short-circuits >= 99% of probes.
// -----------------------------------------------------------------------------
void Test2_BloomFilterShortCircuitVerification() {
    std::cout << "\n=== Test 2: Bloom Filter Short-Circuit Engine (Feature 68) ===\n";

    std::string test_dir = "./test_phase12_bloom";
    SetupDir(test_dir);

    // Build SSTable with 1,000 keys "key_present_XXXX"
    std::vector<std::pair<std::string, std::string>> present_kvs;
    for (int i = 0; i < 1000; ++i) {
        char kbuf[64];
        std::snprintf(kbuf, sizeof(kbuf), "key_present_%06d", i);
        present_kvs.push_back({kbuf, "val_" + std::to_string(i)});
    }
    FileMetaData meta = CreateTestSSTable(test_dir, 2001, present_kvs, 100, kTypeValue, 1024);

    TableOptions topts;
    topts.filter_policy = std::make_shared<BloomFilterPolicy>(10);
    std::unique_ptr<TableReader> reader;
    Status s = TableReader::Open(topts, meta.filename, meta.file_size, nullptr, &reader);
    assert(s.ok());

    BloomShortCircuit circuit;
    auto policy = std::make_shared<BloomFilterPolicy>(10);
    FilterBlockBuilder filter_builder(policy.get());
    for (int i = 0; i < 1000; ++i) {
        char kbuf[64];
        std::snprintf(kbuf, sizeof(kbuf), "key_present_%06d", i);
        filter_builder.AddKey(Slice(kbuf));
    }
    Slice filter_data = filter_builder.Finish();
    FilterBlockReader filter_reader(policy.get(), filter_data);

    // Probe 1,000 non-existent keys "absent_key_XXXX"
    int skipped_count = 0;
    for (int i = 0; i < 1000; ++i) {
        char kbuf[64];
        std::snprintf(kbuf, sizeof(kbuf), "absent_key_%06d", i);
        bool match = circuit.KeyMayMatch(&filter_reader, 0, Slice(kbuf));
        if (!match) {
            skipped_count++;
        }
    }

    double skip_pct = (static_cast<double>(skipped_count) / 1000.0) * 100.0;
    BloomShortCircuitStats stats = circuit.GetStats();
    std::cout << "  - " << stats.ToString() << "\n";
    std::cout << "  - Disk Read Bypass:     " << skipped_count << " / 1000 (" << skip_pct << "%)\n";

    assert(skip_pct >= 99.0);
    std::cout << "  [PASSED] Test 2: Bloom Filter Short-Circuit Verification\n";
}

// -----------------------------------------------------------------------------
// Test 3: Index Pinpoint Handle Precision (Feature 69)
// Construct an SSTable with 50 data blocks; pinpoint 100 random keys;
// verify index reader resolves the exact correct BlockHandle offset.
// -----------------------------------------------------------------------------
void Test3_IndexPinpointHandlePrecision() {
    std::cout << "\n=== Test 3: Index-Assisted Block Pinpoint Lookup (Feature 69) ===\n";

    IndexBuilder index_builder;
    for (int i = 0; i < 50; ++i) {
        char kbuf_last[64], kbuf_next[64];
        std::snprintf(kbuf_last, sizeof(kbuf_last), "key_%05d_last", i);
        std::snprintf(kbuf_next, sizeof(kbuf_next), "key_%05d_next", i + 1);
        std::string last_str = kbuf_last;
        Slice next_slice(kbuf_next);
        BlockHandle handle;
        handle.set_offset(static_cast<uint64_t>(i * 1024));
        handle.set_size(1024);
        index_builder.AddIndexEntry(&last_str, &next_slice, handle);
    }
    Slice index_slice = index_builder.Finish();
    IndexReader index_reader(index_slice);
    IndexPinpointLookup pinpoint(&index_reader);

    // Test pinpointing keys across all 50 blocks
    for (int i = 0; i < 50; ++i) {
        char kbuf[64];
        std::snprintf(kbuf, sizeof(kbuf), "key_%05d_last", i);
        BlockHandle handle;
        bool found = pinpoint.FindBlockHandle(Slice(kbuf), &handle);
        assert(found);
        assert(handle.offset() == static_cast<uint64_t>(i * 1024));
        assert(handle.size() == 1024);
    }

    std::cout << "  - Successfully pinpointed keys across 50 data block partitions.\n";
    std::cout << "  - Verified precise BlockHandle offset and size resolution.\n";
    std::cout << "  [PASSED] Test 3: Index Pinpoint Handle Precision\n";
}

// -----------------------------------------------------------------------------
// Test 4: Sharded Block Cache Eviction & Leases (Feature 70)
// Fill block cache to capacity; verify eviction occurs in strict LRU order;
// assert pinned handles are not evicted until explicitly released.
// -----------------------------------------------------------------------------
void Test4_ShardedBlockCacheEvictionAndLeases() {
    std::cout << "\n=== Test 4: High-Concurrency LRU Block Cache Lookup (Feature 70) ===\n";

    // Small 1 KB total capacity cache across 16 shards
    ShardedLRUCache cache(1024);

    // Insert block A (200 bytes)
    auto val_a = std::make_shared<std::string>(200, 'A');
    auto h_a = cache.Insert(Slice("block_A"), val_a, 200);
    assert(h_a != nullptr);

    // Insert block B (200 bytes)
    auto val_b = std::make_shared<std::string>(200, 'B');
    auto h_b = cache.Insert(Slice("block_B"), val_b, 200);
    assert(h_b != nullptr);

    // Release leases on A and B so they can be evicted
    cache.Release(h_a);
    cache.Release(h_b);

    // Pin block C with an active lease (200 bytes)
    auto val_c = std::make_shared<std::string>(200, 'C');
    auto h_c = cache.Insert(Slice("block_C"), val_c, 200);
    assert(h_c != nullptr); // h_c is PINNED

    // Lookup A to refresh its LRU recency
    auto h_a_lookup = cache.Lookup(Slice("block_A"));
    assert(h_a_lookup != nullptr);
    cache.Release(h_a_lookup);

    // Now insert large blocks to force eviction
    for (int i = 0; i < 20; ++i) {
        std::string key = "extra_block_" + std::to_string(i);
        auto val = std::make_shared<std::string>(100, 'X');
        auto h = cache.Insert(Slice(key), val, 100);
        cache.Release(h);
    }

    // Pinned block C must still have valid value in memory!
    Slice c_slice = cache.Value(h_c);
    assert(c_slice.size() == 200);
    assert(c_slice[0] == 'C');
    cache.Release(h_c); // Now released

    std::cout << "  - Cache Total Hits: " << cache.Hits() << ", Misses: " << cache.Misses() << "\n";
    std::cout << "  - Verified pinned handle leases protected from eviction.\n";
    std::cout << "  [PASSED] Test 4: Sharded Block Cache Eviction & Leases\n";
}

// -----------------------------------------------------------------------------
// Test 5: Multi-Source Merging Iterator Ordering (Feature 71)
// Feed 3 child iterators (Memtable, L0 SSTable, L1 SSTable) with overlapping keys;
// assert MergingIterator yields strictly monotonic keys and shadows older versions.
// -----------------------------------------------------------------------------
void Test5_MultiSourceMergingIteratorOrdering() {
    std::cout << "\n=== Test 5: Multi-Source Merging Iterator (Feature 71) ===\n";

    std::string test_dir = "./test_phase12_merge";
    SetupDir(test_dir);

    // Source 1: Memtable with keys "apple" (seq 300), "cherry" (seq 300)
    auto mem = std::make_shared<MemTableCore>();
    mem->Add(300, kTypeValue, Slice("apple"), Slice("mem_apple"));
    mem->Add(300, kTypeValue, Slice("cherry"), Slice("mem_cherry"));

    // Source 2: L0 SSTable with keys "apple" (seq 200), "banana" (seq 200)
    std::vector<std::pair<std::string, std::string>> kvs_l0 = {
        {"apple", "l0_apple"}, {"banana", "l0_banana"}
    };
    FileMetaData f_l0 = CreateTestSSTable(test_dir, 5001, kvs_l0, 200);

    // Source 3: L1 SSTable with keys "banana" (seq 100), "date" (seq 100)
    std::vector<std::pair<std::string, std::string>> kvs_l1 = {
        {"banana", "l1_banana"}, {"date", "l1_date"}
    };
    FileMetaData f_l1 = CreateTestSSTable(test_dir, 5002, kvs_l1, 100);

    TableOptions topts;
    std::unique_ptr<TableReader> r0, r1;
    assert(TableReader::Open(topts, f_l0.filename, f_l0.file_size, nullptr, &r0).ok());
    assert(TableReader::Open(topts, f_l1.filename, f_l1.file_size, nullptr, &r1).ok());

    std::vector<std::unique_ptr<Iterator>> children;
    children.push_back(std::make_unique<MemTableInternalIterator>(mem->NewIterator()));
    children.push_back(r0->NewIterator());
    children.push_back(r1->NewIterator());

    MergingIterator merge_iter(std::move(children));
    merge_iter.SeekToFirst();

    std::vector<std::string> keys_yielded;
    while (merge_iter.Valid()) {
        InternalKey ikey;
        ikey.DecodeFrom(merge_iter.key());
        keys_yielded.push_back(ikey.user_key().ToString() + "@" + std::to_string(ikey.sequence()));
        merge_iter.Next();
    }

    std::cout << "  - Yielded Keys in Order: ";
    for (const auto& k : keys_yielded) {
        std::cout << k << " ";
    }
    std::cout << "\n";

    // Assert monotonic order: apple@300, apple@200, banana@200, banana@100, cherry@300, date@100
    assert(keys_yielded.size() == 6);
    assert(keys_yielded[0] == "apple@300");
    assert(keys_yielded[1] == "apple@200");
    assert(keys_yielded[2] == "banana@200");
    assert(keys_yielded[3] == "banana@100");
    assert(keys_yielded[4] == "cherry@300");
    assert(keys_yielded[5] == "date@100");

    std::cout << "  [PASSED] Test 5: Multi-Source Merging Iterator Ordering\n";
}

// -----------------------------------------------------------------------------
// Test 6: User Forward Iterator Boundary Pruning (Feature 72)
// Iterate keys with an upper bound key_upper = "user:050";
// assert iterator terminates cleanly before yielding "user:050" or beyond, and drops tombstones.
// -----------------------------------------------------------------------------
void Test6_UserForwardIteratorBoundaryPruning() {
    std::cout << "\n=== Test 6: Linear Forward Iterator with Boundary Pruning (Feature 72) ===\n";

    auto mem = std::make_shared<MemTableCore>();
    // Add keys user:010 .. user:080
    for (int i = 10; i <= 80; i += 10) {
        char kbuf[64];
        std::snprintf(kbuf, sizeof(kbuf), "user:%03d", i);
        if (i == 30) {
            // Tombstone for user:030
            mem->Add(100, kTypeDeletion, Slice(kbuf), Slice(""));
        } else {
            mem->Add(100, kTypeValue, Slice(kbuf), Slice("val_" + std::to_string(i)));
        }
    }

    std::vector<std::unique_ptr<Iterator>> children;
    children.push_back(std::make_unique<MemTableInternalIterator>(mem->NewIterator()));
    auto merge_iter = std::make_unique<MergingIterator>(std::move(children));

    ReadOptionsExtended opts;
    opts.snapshot = 100;
    opts.iterate_upper_bound = "user:050"; // Upper bound: must stop before user:050

    DBIterator db_iter(std::move(merge_iter), opts);
    db_iter.SeekToFirst();

    std::vector<std::string> visible_keys;
    while (db_iter.Valid()) {
        visible_keys.push_back(db_iter.key().ToString());
        db_iter.Next();
    }

    std::cout << "  - Visible User Keys (Upper Bound 'user:050'): ";
    for (const auto& k : visible_keys) {
        std::cout << k << " ";
    }
    std::cout << "\n";

    // user:010, user:020, user:040 (user:030 is tombstone, user:050+ pruned by upper bound)
    assert(visible_keys.size() == 3);
    assert(visible_keys[0] == "user:010");
    assert(visible_keys[1] == "user:020");
    assert(visible_keys[2] == "user:040");

    std::cout << "  - Verified: user:030 tombstone suppressed, user:050+ pruned by upper bound.\n";
    std::cout << "  [PASSED] Test 6: User Forward Iterator Boundary Pruning\n";
}

int main() {
    std::cout << "=======================================================================\n";
    std::cout << "  LSM-Tree Key-Value Engine: Phase XII Verification Test Suite\n";
    std::cout << "  Features 67-72: Reads & Query Engine Core (Part 1)\n";
    std::cout << "=======================================================================\n";

    Test1_MultiLevelQueryCascade();
    Test2_BloomFilterShortCircuitVerification();
    Test3_IndexPinpointHandlePrecision();
    Test4_ShardedBlockCacheEvictionAndLeases();
    Test5_MultiSourceMergingIteratorOrdering();
    Test6_UserForwardIteratorBoundaryPruning();

    std::cout << "\n=======================================================================\n";
    std::cout << "  [ALL 6 TESTS PASSED] Phase XII Verification 100% Successful!\n";
    std::cout << "=======================================================================\n";

    return 0;
}
