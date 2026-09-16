# Benchmarks

`bash ci/build.sh benchmark` produces four JSON reports in `artifacts/`.
The storage-engine benchmark runs 8 MiB streams with 4 KiB, 64 KiB, and 1 MiB
chunks, then 100 random 4 KiB overwrites and four concurrent writers. All writes
use durable transactions and retain version history. The sequential workload
repeats a deterministic pseudorandom 1 MiB block, deliberately exercising
deduplication. Read passes validate their output. The second pass uses the
bounded plaintext cache when the working set fits. This is not an uncached disk
speed measurement.

`benchmark-fuse.json` measures a separate workload through the real mounted
filesystem: 8 MiB of seeded pseudorandom data, 64 KiB application writes, fsync,
and a verified sequential read. Each run initializes a disposable encrypted
store on WSL's native Linux filesystem. The store and daemon are cleaned up.

Reports include MiB/s, random-write operations per second, unique chunk and
reference counts, stored bytes, and engine cache hit/miss counts. Throughput is
machine- and workload-specific. Small chunks reduce overwrite amplification but
increase metadata work. Larger chunks can reduce metadata overhead while making
small overwrites more expensive. Retaining every write version also grows
metadata and keeps old chunks live; prune and GC are explicit administration.

To change the engine workload:

```bash
./build/fusefs-bench --bytes 67108864 --chunk-size 65536
```

Benchmarks run in Jenkins after correctness and sanitizer tests. They verify
execution and output but deliberately impose no machine-specific performance
threshold. Compare matching workloads on the same host across revisions.
