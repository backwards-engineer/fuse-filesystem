import concurrent.futures
import ctypes
import errno
import fcntl
import json
import os
from pathlib import Path
import random
import select
import sqlite3
import stat
import subprocess
import sys
import threading
import unittest
from mount_support import MountedStore

BINARY, ADMIN = sys.argv[1:3]
del sys.argv[1:3]


class FuseTests(unittest.TestCase):
    def setUp(self):
        self.fs = MountedStore(BINARY, ADMIN)
        self.addCleanup(self.fs.close)
        self.fs.start()
        self.m = self.fs.mount

    def test_streaming_offsets_and_fsync(self):
        payload = random.Random(99).randbytes(2 * 1024 * 1024 + 731)
        fd = os.open(self.m / "stream", os.O_CREAT | os.O_RDWR, 0o640)
        try:
            for offset in range(0, len(payload), 17003):
                block = payload[offset:offset + 17003]
                self.assertEqual(os.pwrite(fd, block, offset), len(block))
            os.fsync(fd)
            result = b"".join(os.pread(fd, 3109, off) for off in range(0, len(payload), 3109))
            self.assertEqual(result, payload)
            self.assertEqual(os.pread(fd, 10, len(payload)), b"")
        finally:
            os.close(fd)

    def test_sparse_seek_and_punch(self):
        fd = os.open(self.m / "sparse", os.O_CREAT | os.O_RDWR, 0o600)
        try:
            end = 1 << 40
            os.pwrite(fd, b"tail", end)
            self.assertEqual(os.fstat(fd).st_size, end + 4)
            self.assertEqual(os.fstat(fd).st_blocks, 8)
            self.assertEqual(os.pread(fd, 100, 1 << 30), bytes(100))
            self.assertEqual(os.lseek(fd, 0, os.SEEK_DATA), end)
            self.assertEqual(os.lseek(fd, 0, os.SEEK_HOLE), 0)
            libc = ctypes.CDLL(None, use_errno=True)
            libc.fallocate.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_longlong, ctypes.c_longlong]
            self.assertEqual(libc.fallocate(fd, 3, end, 4096), 0)
            self.assertEqual(os.pread(fd, 4, end), bytes(4))
            self.assertEqual(os.fstat(fd).st_blocks, 0)
        finally:
            os.close(fd)

    def test_truncate_clears_hidden_tail(self):
        p = self.m / "truncate"
        p.write_bytes(b"abcdef")
        os.truncate(p, 2)
        os.truncate(p, 9000)
        self.assertEqual(p.read_bytes(), b"ab" + bytes(8998))
        with p.open("wb") as output:
            output.write(b"x")
        self.assertEqual(p.read_bytes(), b"x")

    def test_permissions_and_ownership(self):
        self.assertNotEqual(os.geteuid(), 0, "Permission tests must run as a regular Linux user")
        p = self.m / "private"
        p.write_bytes(b"secret")
        p.chmod(0)
        with self.assertRaises(PermissionError):
            p.read_bytes()
        p.chmod(0o640)
        self.assertEqual(stat.S_IMODE(p.stat().st_mode), 0o640)
        os.chown(p, os.getuid(), os.getgid())
        with self.assertRaises(PermissionError):
            os.chown(p, os.getuid() + 1, os.getgid())
        self.assertEqual(p.stat().st_uid, os.getuid())
        d = self.m / "restricted"
        d.mkdir(mode=0o700)
        d.chmod(0o600)
        with self.assertRaises(PermissionError):
            (d / "child").write_bytes(b"bad")
        d.chmod(0o700)

    def test_umask_and_timestamps(self):
        previous = os.umask(0o027)
        try:
            fd = os.open(self.m / "masked", os.O_CREAT | os.O_WRONLY, 0o666)
            os.close(fd)
            (self.m / "dir").mkdir(mode=0o777)
        finally:
            os.umask(previous)
        self.assertEqual(stat.S_IMODE((self.m / "masked").stat().st_mode), 0o640)
        self.assertEqual(stat.S_IMODE((self.m / "dir").stat().st_mode), 0o750)
        os.utime(self.m / "masked", ns=(123456789012345678, 223456789012345678))
        self.assertEqual((self.m / "masked").stat().st_mtime_ns, 223456789012345678)

    def test_symlinks_and_dangling_links(self):
        (self.m / "target").write_bytes(b"value")
        os.symlink("target", self.m / "link")
        self.assertEqual(os.readlink(self.m / "link"), "target")
        self.assertEqual((self.m / "link").read_bytes(), b"value")
        os.symlink("absent", self.m / "dangling")
        self.assertTrue((self.m / "dangling").is_symlink())
        with self.assertRaises(FileNotFoundError):
            (self.m / "dangling").read_bytes()
        (self.m / "dir").mkdir()
        (self.m / "dir" / "file").write_bytes(b"nested")
        os.symlink("dir", self.m / "dirlink")
        self.assertEqual((self.m / "dirlink" / "file").read_bytes(), b"nested")

    def test_atomic_rename_and_open_destination(self):
        (self.m / "old").write_bytes(b"old")
        (self.m / "new").write_bytes(b"new")
        with (self.m / "old").open("rb", buffering=0) as old:
            os.replace(self.m / "new", self.m / "old")
            self.assertEqual(old.read(), b"old")
            self.assertEqual((self.m / "old").read_bytes(), b"new")
        self.assertFalse((self.m / "new").exists())

    def test_rename_flags_and_directory_cycles(self):
        (self.m / "a").write_bytes(b"a")
        (self.m / "b").write_bytes(b"b")
        libc = ctypes.CDLL(None, use_errno=True)
        libc.renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        a, b = os.fsencode(self.m / "a"), os.fsencode(self.m / "b")
        self.assertEqual(libc.renameat2(-100, a, -100, b, 1), -1)
        self.assertEqual(ctypes.get_errno(), errno.EEXIST)
        self.assertEqual(libc.renameat2(-100, a, -100, b, 2), 0)
        self.assertEqual((self.m / "a").read_bytes(), b"b")
        self.assertEqual((self.m / "b").read_bytes(), b"a")
        (self.m / "dir").mkdir()
        (self.m / "dir" / "sub").mkdir()
        with self.assertRaises(OSError):
            os.rename(self.m / "dir", self.m / "dir" / "sub" / "cycle")

    def test_rename_observers_see_complete_files(self):
        p = self.m / "published"
        p.write_bytes(b"A" * 4000)
        stopped = threading.Event()

        def reader():
            while not stopped.is_set():
                self.assertIn(p.read_bytes(), (b"A" * 4000, b"B" * 4000))

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            future = pool.submit(reader)
            try:
                for i in range(30):
                    temp = self.m / "temp"
                    temp.write_bytes((b"A" if i % 2 else b"B") * 4000)
                    os.replace(temp, p)
            finally:
                stopped.set()
            future.result(timeout=15)

    def test_open_unlink_lifetime(self):
        p = self.m / "open"
        fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o600)
        try:
            p.unlink()
            self.assertFalse(p.exists())
            os.write(fd, b"still open")
            self.assertEqual(os.pread(fd, 20, 0), b"still open")
            self.assertEqual(os.fstat(fd).st_nlink, 0)
        finally:
            os.close(fd)

    def test_multithreaded_random_access(self):
        p = self.m / "concurrent"
        fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o600)
        try:
            def worker(index):
                data = bytes([index + 1]) * 4096
                for _ in range(20):
                    self.assertEqual(os.pwrite(fd, data, index * 4096), len(data))
                    self.assertEqual(os.pread(fd, 4096, index * 4096), data)
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(worker, range(8)))
            self.assertEqual(p.read_bytes(), b"".join(bytes([i + 1]) * 4096 for i in range(8)))
        finally:
            os.close(fd)

    def test_atomic_append(self):
        p = self.m / "append"
        def worker(index):
            fd = os.open(p, os.O_CREAT | os.O_APPEND | os.O_WRONLY, 0o600)
            try:
                for i in range(30):
                    record = f"{index:02}:{i:03}".encode().ljust(64, b".")
                    self.assertEqual(os.write(fd, record), 64)
            finally:
                os.close(fd)
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            list(pool.map(worker, range(6)))
        data = p.read_bytes()
        self.assertEqual(len(data), 6 * 30 * 64)
        self.assertEqual(len({data[i:i + 64] for i in range(0, len(data), 64)}), 180)

    def test_posix_byte_range_locks(self):
        p = self.m / "locked"
        p.write_bytes(bytes(100))
        script = """import errno, fcntl, sys
with open(sys.argv[1], 'r+b') as f:
    try:
        fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, int(sys.argv[2]))
    except OSError as e:
        sys.exit(23 if e.errno in (errno.EAGAIN, errno.EACCES) else 99)
"""
        with p.open("r+b") as stream:
            fcntl.lockf(stream, fcntl.LOCK_EX, 10, 0)
            blocked = subprocess.run([sys.executable, "-c", script, str(p), "0"], timeout=10)
            allowed = subprocess.run([sys.executable, "-c", script, str(p), "20"], timeout=10)
            self.assertEqual(blocked.returncode, 23)
            self.assertEqual(allowed.returncode, 0)
            fcntl.lockf(stream, fcntl.LOCK_UN, 10, 0)
        self.assertEqual(subprocess.run([sys.executable, "-c", script, str(p), "0"], timeout=10).returncode, 0)

    def test_flock_blocking_and_release(self):
        p = self.m / "flock"
        p.write_bytes(b"lock")
        script = """import fcntl, sys
with open(sys.argv[1], 'r+b') as f:
    print('ready', flush=True)
    fcntl.flock(f, fcntl.LOCK_EX)
    print('acquired', flush=True)
"""
        with p.open("r+b") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX)
            proc = subprocess.Popen([sys.executable, "-c", script, str(p)], stdout=subprocess.PIPE, text=True)
            try:
                self.assertTrue(select.select([proc.stdout], [], [], 5)[0])
                self.assertEqual(proc.stdout.readline().strip(), "ready")
                self.assertIsNone(proc.poll())
                fcntl.flock(stream, fcntl.LOCK_UN)
                output, _ = proc.communicate(timeout=5)
                self.assertIn("acquired", output)
                self.assertEqual(proc.returncode, 0)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)
                proc.stdout.close()

    def test_open_file_description_locks(self):
        class Flock(ctypes.Structure):
            _fields_ = [("kind", ctypes.c_short), ("whence", ctypes.c_short),
                        ("start", ctypes.c_longlong), ("length", ctypes.c_longlong),
                        ("pid", ctypes.c_int)]
        p = self.m / "ofd"
        p.write_bytes(bytes(100))
        first = os.open(p, os.O_RDWR)
        second = os.open(p, os.O_RDWR)
        try:
            lock = bytes(Flock(fcntl.F_WRLCK, os.SEEK_SET, 0, 10, 0))
            fcntl.fcntl(first, fcntl.F_OFD_SETLK, lock)
            with self.assertRaises(OSError) as error:
                fcntl.fcntl(second, fcntl.F_OFD_SETLK, lock)
            self.assertIn(error.exception.errno, (errno.EAGAIN, errno.EACCES))
            os.close(first)
            first = -1
            fcntl.fcntl(second, fcntl.F_OFD_SETLK, lock)
        finally:
            if first != -1:
                os.close(first)
            os.close(second)

    def test_remount_persistence(self):
        p = self.m / "persistent"
        p.write_bytes(b"persist" * 3000)
        p.chmod(0o600)
        self.fs.stop()
        self.fs.start()
        self.assertEqual(p.read_bytes(), b"persist" * 3000)
        self.assertEqual(stat.S_IMODE(p.stat().st_mode), 0o600)

    def test_sigkill_after_acknowledged_write(self):
        p = self.m / "durable"
        with p.open("wb", buffering=0) as stream:
            stream.write(b"durable payload" * 1000)
            os.fsync(stream.fileno())
        self.fs.stop(crash=True)
        self.fs.admin("fsck")
        self.fs.start()
        self.assertEqual(p.read_bytes(), b"durable payload" * 1000)

    def test_cli_history_restore_prune_gc(self):
        p = self.m / "versioned"
        p.write_bytes(b"first")
        self.fs.stop()
        old = json.loads(self.fs.admin("history", "--path", "/versioned").stdout)[0]["id"]
        self.fs.start()
        p.write_bytes(b"second")
        self.fs.stop()
        self.assertEqual(self.fs.admin("cat", "--path", "/versioned", "--version", old).stdout, b"first")
        self.fs.admin("restore", "--path", "/versioned", "--version", old)
        self.fs.admin("prune", "--keep", 1)
        self.fs.admin("gc")
        self.fs.admin("fsck")
        self.assertEqual(self.fs.admin("cat", "--path", "/versioned").stdout, b"first")
        self.assertEqual(len(json.loads(self.fs.admin("history", "--path", "/versioned").stdout)), 1)

    def test_cli_import_export_stream(self):
        self.fs.stop()
        payload = random.Random(21).randbytes(1048576 + 123)
        source = self.fs.root / "input"
        source.write_bytes(payload)
        self.fs.admin("put", "--path", "/import", "--input", source)
        self.assertEqual(self.fs.admin("cat", "--path", "/import").stdout, payload)
        self.fs.admin("fsck")

    def test_corruption_fails_closed(self):
        (self.m / "corrupt").write_bytes(b"authenticated" * 100)
        self.fs.stop()
        with sqlite3.connect(self.fs.store / "store.db") as db:
            db.execute("UPDATE chunks SET data=zeroblob(length(data))")
        self.assertEqual(self.fs.admin("fsck", check=False).returncode, 2)
        self.fs.start()
        with self.assertRaises(OSError) as error:
            (self.m / "corrupt").read_bytes()
        self.assertEqual(error.exception.errno, errno.EIO)

    def test_exclusive_admin_and_wrong_key(self):
        result = self.fs.admin("stats", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"already mounted", result.stderr)
        self.fs.stop()
        wrong = self.fs.root / "wrong-key"
        wrong.write_bytes(os.urandom(32))
        wrong.chmod(0o600)
        result = self.fs.admin("stats", key=wrong, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"Wrong key", result.stderr)

    def test_readonly_mount(self):
        (self.m / "data").write_bytes(b"read")
        self.fs.stop()
        self.fs.start("-o", "ro")
        self.assertEqual((self.m / "data").read_bytes(), b"read")
        with self.assertRaises(OSError) as error:
            (self.m / "data").write_bytes(b"bad")
        self.assertEqual(error.exception.errno, errno.EROFS)

    def test_directory_unicode_and_errors(self):
        d = self.m / "café"
        d.mkdir()
        (d / "文件").write_bytes(b"utf8")
        self.assertEqual([p.name for p in d.iterdir()], ["文件"])
        with self.assertRaises(OSError) as error:
            d.rmdir()
        self.assertEqual(error.exception.errno, errno.ENOTEMPTY)
        with self.assertRaises(FileNotFoundError):
            (d / "missing").read_bytes()


if __name__ == "__main__":
    unittest.main(verbosity=2)
