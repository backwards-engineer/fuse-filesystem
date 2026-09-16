# Storage format and guarantees

Format 1 uses SQLite WAL with `synchronous=FULL`. Every mutation commits its
metadata, immutable chunk references, and encrypted chunk objects in one database
transaction. Failed operations roll back. A process crash leaves either the old
or new transaction visible. Physical power-loss durability depends on the Linux
filesystem and device honoring SQLite's sync operations. Use a local Linux
filesystem for the store, including WSL's ext4 virtual disk.

Each write callback is a transaction and creates a content version. A large
application write can span multiple FUSE requests; use write-temp + fsync + rename
for atomic whole-file replacement. Rename supports replacement, NOREPLACE and
EXCHANGE. Open handles address inodes, so renaming or unlinking does not break
existing handles. One process owns a store at a time. Within a process, a mutex
serializes operations and SQLite transactions; this is thread-safe, not a claim
of linear write scaling. FUSE runs multiple worker threads by default.

Chunks are fixed-size, padded with zeroes, and addressed by plaintext SHA-256.
All-zero chunks are holes. Chunk size is selected at initialization, from 4 KiB
to 4 MiB in powers of two, and cannot change for an existing store. Unaligned I/O
updates only the touched chunks. Storage buffers are bounded by request and
chunk size, not file size. SEEK_DATA/SEEK_HOLE operate at chunk granularity.
Hole punching is supported; space reservation with other fallocate modes is not.

Compression uses Zstandard, only when it reduces size. Level 0 disables it.
AES-256-GCM encrypts each unique chunk using a fresh random 96-bit nonce and a
128-bit tag. Authenticated associated data binds format, hash, codec, and raw
length. Reads authenticate, decompress with a fixed bound, and verify SHA-256.
Version manifests also have HMAC-SHA-256 authentication over the inode, size,
timestamp and ordered chunk mapping, so missing or substituted references are
detected rather than silently turning into holes. Manifest hashing is streamed.
Deduplication is within one store. Encryption protects file contents at rest;
filenames, symlink targets, sizes, ownership, history metadata and equality of
chunks are visible in SQLite. Namespace and Unix ownership metadata are
transactionally consistent but not cryptographically authenticated. Rollback
to an older complete store is not detectable without an external trusted record.

Keys are random 32-byte files, outside the store, owned by the mounting user and
mode 0600. They are never written to the database or logs. The store directory
must be private (0700). Back up the store and key separately while unmounted.
Losing the key loses access to content; key rotation is not implemented.

An in-memory LRU caches verified plaintext chunks with a configurable byte limit
(`--cache-mib`, 0 disables it). Entries are cleansed on eviction and shutdown.
The cache is not persisted. Corruption introduced after a cached read may be
noticed only on a cache miss or by fsck, which always bypasses the cache.

Linux enforces Unix mode bits and uid/gid using `default_permissions`. Creation
honors umask and setgid directories; writes/chown clear setuid/setgid. Symlinks
are resolved by the kernel. Linux provides local advisory POSIX byte-range,
open-file-description, and BSD flock locks. Locks are not distributed or
mandatory. POSIX ACLs, extended attributes, hardlinks, device nodes, and writable
mmap are not provided. Atime updates happen through utimens, not on every read.

Versions preserve regular-file contents and size after each write, truncate,
hole punch, and restore; they are not whole-filesystem snapshots. Metadata
operations do not create content versions. Deleted inode history remains until
`gc --purge-deleted`; use `history --inode ID` to inspect it. `prune --keep N`
retains the newest N versions and always the current version. GC removes only
unreferenced encrypted chunks and compacts SQLite. These administration commands
require an unmounted store and obtain the same exclusive store lock as a mount.

`fsck` is read-only with respect to stored data: it checks SQLite structure,
foreign keys, namespace reachability/cycles, current versions, chunk ranges,
authentication and content hashes. It reports corruption and exits nonzero;
it does not attempt destructive repair. Unmount, retain a backup, and restore
from a known-good backup when corruption is reported.
