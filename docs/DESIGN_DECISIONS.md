# Design Decisions & Architectural Trade-offs: LSM-Tree Key-Value Engine

## Stack Selection
- **Core Technology**: `Rust, C++, Python, React / TypeScript`
- **Rationale**: Strict latency bounds, memory safety, and cross-platform determinism required for high-throughput systems infrastructure.

## Key Trade-offs
1. **Memory Pre-allocation vs Dynamic Sizing**:
   - *Decision*: Memory buffers and state pools are pre-allocated at initialization.
   - *Trade-off*: Slightly higher initialization memory footprint in exchange for zero garbage collection pauses and zero allocator contention on the critical path.
2. **Synchronous Core vs Asynchronous IO**:
   - *Decision*: IO boundaries use asynchronous non-blocking event loops, while core execution kernels run synchronously on dedicated worker threads.
   - *Trade-off*: Cleaner separation of concerns with predictable execution latency.
