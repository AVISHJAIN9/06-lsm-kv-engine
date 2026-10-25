# 06 Api Reference: LSM-Tree Key-Value Engine

## 1. Specification Overview [Specified]
RocksDB-style embedded key-value storage engine with leveled compaction and write-amplification accounting.

## 2. Core Mechanisms [Specified]
WAL with fsync policy and checksums, skiplist memtable, SSTable format (data blocks, index, bloom filter, prefix compression), leveled & size-tiered compaction with write-amplification accounting, MVCC sequence numbers, snapshots, iterators, tombstone GC, block cache, manifest.

## 3. Unknowns & Benchmarks
All performance numbers, throughput rates, and latency figures are marked **TO BE MEASURED** until run on standard hardware.
