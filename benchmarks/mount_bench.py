"""Benchmark the actual kernel/FUSE path separately from the storage engine."""
import json
from pathlib import Path
import os
import random
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from mount_support import MountedStore

fs = MountedStore(sys.argv[1], sys.argv[2], chunk_size=65536)
try:
    fs.start()
    payload = random.Random(12345).randbytes(8 * 1024 * 1024)
    path = fs.mount / "benchmark"
    started = time.perf_counter()
    with path.open("wb", buffering=0) as stream:
        for offset in range(0, len(payload), 65536):
            stream.write(payload[offset:offset + 65536])
        os.fsync(stream.fileno())
    write_time = time.perf_counter() - started
    started = time.perf_counter()
    actual = path.read_bytes()
    read_time = time.perf_counter() - started
    if actual != payload:
        raise RuntimeError("Benchmark read verification failed")
    fs.stop()
    stats = json.loads(fs.admin("stats").stdout)
    print(json.dumps({"path": "kernel-fuse", "bytes": len(payload), "chunk_size": 65536,
                      "write_mib_s": 8 / write_time, "read_mib_s": 8 / read_time,
                      "storage": stats}))
finally:
    fs.close()
