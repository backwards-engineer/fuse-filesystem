#!/usr/bin/env bash
set -euo pipefail
binary=$(realpath "${1:-build/local-fusefs}")
[[ -c /dev/fuse ]] || { echo 'ERROR: /dev/fuse is unavailable; mount tests are required.' >&2; exit 1; }
mount_dir=$(mktemp -d)
fs_pid=''
cleanup() {
  if mountpoint -q "$mount_dir"; then fusermount3 -u "$mount_dir"; fi
  if [[ -n "$fs_pid" ]]; then kill "$fs_pid" 2>/dev/null || true; wait "$fs_pid" 2>/dev/null || true; fi
  rmdir "$mount_dir"
}
trap cleanup EXIT
"$binary" -f -s -o ro "$mount_dir" &
fs_pid=$!
for i in $(seq 1 100); do
  mountpoint -q "$mount_dir" && break
  kill -0 "$fs_pid" || { echo 'Filesystem exited before mounting' >&2; exit 1; }
  sleep 0.1
done
mountpoint -q "$mount_dir" || { echo 'Mount timeout' >&2; exit 1; }
python3 - "$mount_dir" <<'PY'
import errno
import os
import pathlib
import sys
p = pathlib.Path(sys.argv[1])
assert sorted(x.name for x in p.iterdir()) == ['hello.txt']
assert (p / 'hello.txt').read_bytes() == b'Hello from local-fusefs!\n'
with (p / 'hello.txt').open('rb') as stream:
    stream.seek(6)
    assert stream.read(4) == b'from'
    stream.seek(10000)
    assert stream.read() == b''
assert (p / 'hello.txt').stat().st_size == len(b'Hello from local-fusefs!\n')
try:
    (p / 'missing').read_bytes()
except FileNotFoundError:
    pass
else:
    raise AssertionError('Missing file was readable')
try:
    (p / 'hello.txt').write_bytes(b'bad')
except OSError as error:
    assert error.errno in (errno.EROFS, errno.EACCES)
else:
    raise AssertionError('Read-only file accepted a write')
print('Real FUSE mount, directory, read, seek, EOF, missing-file, and write-rejection checks passed.')
PY
