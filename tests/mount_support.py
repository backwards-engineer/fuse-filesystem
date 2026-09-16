"""Real FUSE fixture, using private native-Linux temporary directories."""
import os
from pathlib import Path
import subprocess
import tempfile
import time


class MountedStore:
    def __init__(self, binary, admin, chunk_size=4096):
        self.binary = str(Path(binary).resolve())
        self.admin_binary = str(Path(admin).resolve())
        self.temporary = tempfile.TemporaryDirectory(prefix="local-fusefs-mount-")
        self.root = Path(self.temporary.name)
        self.store = self.root / "store"
        self.key = self.root / "key"
        self.mount = self.root / "mount"
        self.mount.mkdir()
        self.process = None
        self.log = None
        self.admin("init", "--chunk-size", str(chunk_size))

    def admin(self, command, *args, check=True, key=None):
        return subprocess.run(
            [self.admin_binary, command, str(self.store), "--key-file", str(key or self.key), *map(str, args)],
            capture_output=True, check=check, timeout=30,
        )

    def start(self, *options):
        if not Path("/dev/fuse").is_char_device():
            raise RuntimeError("/dev/fuse is unavailable; these tests must not be skipped")
        self.log = (self.root / "daemon.log").open("ab")
        self.process = subprocess.Popen(
            [self.binary, "--store", str(self.store), "--key-file", str(self.key),
             "--cache-mib", "1", str(self.mount), *options],
            stdout=self.log, stderr=self.log,
        )
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.is_mounted():
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.02)
        raise RuntimeError("FUSE mount failed: " + (self.root / "daemon.log").read_text(errors="replace"))

    def is_mounted(self):
        # stat/ismount fails with ENOTCONN after SIGKILL; mountinfo still records it.
        return any(line.split()[4] == str(self.mount) for line in Path("/proc/self/mountinfo").read_text().splitlines())

    def stop(self, crash=False):
        if self.process is None:
            return
        if crash and self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=10)
        if self.is_mounted():
            subprocess.run(["fusermount3", "-u", "-z", str(self.mount)], check=True, timeout=10)
        try:
            result = self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
            raise
        finally:
            self.process = None
            self.log.close()
            self.log = None
        if not crash and result != 0:
            raise RuntimeError("FUSE exited with errors: " + (self.root / "daemon.log").read_text(errors="replace"))

    def close(self):
        try:
            self.stop()
        finally:
            self.temporary.cleanup()
