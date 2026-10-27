# Systems Engineering Glossary: LSM-Tree Key-Value Engine

### LSM-Tree
Storage architecture optimized for high write throughput by buffering writes in memory and writing sequentially to disk.

### SSTable
Sorted String Table; immutable disk file containing sorted key-value pairs and block index headers.

### Write-Ahead Log (WAL)
Append-only sequential file guaranteeing durability by recording mutations before committing them to the MemTable.

### Leveled Compaction
Background garbage collection process merging overlapping SSTables across exponentially sized tiers.
