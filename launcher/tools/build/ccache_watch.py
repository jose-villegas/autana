"""Run a build and stop it when a ccache lock stays held: ccache itself waits on a held lock forever.

    python ccache_watch.py [--stuck-after SECONDS] -- command [args...]

ccache has no lock timeout: a compile whose lock another process keeps retries every few
milliseconds without end, so the build hangs with no compiler running. ccache holds each lock
only for the milliseconds after a compile, so one that stays held, the same file, for
STUCK_SECONDS is stuck. This stops the command's process tree and fails naming the lock.

The cache is the one `ccache --get-config cache_dir` reports in this environment; with no ccache
on PATH the command runs unwatched. Exits with the command's status, or EXIT_STUCK.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/lib"))
from process_tree import launch_process_tree, stop_process_tree, terminate_unwinds  # noqa: E402

# ccache's log shows each lock held for milliseconds, after the compile.
STUCK_SECONDS = 60
POLL_SECONDS = 2
EXIT_STUCK = 3
# The locks ccache 4.x takes: lock/subdir_XX.lock and auto_cleanup.lock, stats.lock on cache levels 1 and 2.
LOCK_PATTERNS = ("lock/*.lock", "*/stats.lock", "*/*/stats.lock")
# POSIX ccache refreshes a held long-lived lock's <name>.alive and breaks a lock idle for 2 s.
POSIX_ALIVE_SECONDS = 10
ERROR_SHARING_VIOLATION = 32
CCACHE_QUERY_SECONDS = 30


def cache_dir():
    """The ccache cache directory this environment's builds use, or None without ccache."""
    ccache = shutil.which("ccache")
    if not ccache:
        return None
    found = subprocess.run([ccache, "--get-config", "cache_dir"], capture_output=True, text=True,
                           timeout=CCACHE_QUERY_SECONDS)
    return Path(found.stdout.strip()) if found.returncode == 0 and found.stdout.strip() else None


def windows_held(path):
    """Whether a process holds `path` the way ccache does on Windows: opened sharing nothing."""
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    generic_read, share_all, open_existing = 0x80000000, 0x7, 3
    handle = kernel.CreateFileW(str(path), generic_read, share_all, None, open_existing, 0, None)
    if handle == wintypes.HANDLE(-1).value:
        return ctypes.get_last_error() == ERROR_SHARING_VIOLATION
    kernel.CloseHandle(handle)
    return False


def posix_held(path):
    """Whether `path`'s holder still refreshes its keepalive; any other lock a waiter breaks itself."""
    try:
        return time.time() - path.with_suffix(".alive").stat().st_mtime < POSIX_ALIVE_SECONDS
    except OSError:
        return False


def held_locks(cache):
    """Each lock file under `cache` mapped to its identity, which changes when the lock is taken anew."""
    locks = {}
    for pattern in LOCK_PATTERNS:
        for path in cache.glob(pattern):
            try:
                status = path.lstat()
            except OSError:
                continue
            locks[path] = (status.st_ino, status.st_ctime_ns)
    return locks


def is_held(path):
    return windows_held(path) if os.name == "nt" else posix_held(path)


class LockWatch:
    """Remembers since when each lock has been the same file; stuck() names those held past the limit."""

    def __init__(self, cache, stuck_after=STUCK_SECONDS):
        self.cache, self.stuck_after = cache, stuck_after
        self.first_seen = {}

    def stuck(self):
        now = time.monotonic()
        self.first_seen = {lock: self.first_seen.get(lock, now) for lock in held_locks(self.cache).items()}
        return sorted(path for (path, _), since in self.first_seen.items()
                      if now - since >= self.stuck_after and is_held(path))


def run(command, stuck_after=STUCK_SECONDS, poll=POLL_SECONDS):
    """The command's exit status, or EXIT_STUCK after stopping it on a stuck ccache lock."""
    cache = cache_dir()
    if cache is None:
        return subprocess.run(command).returncode
    watch = LockWatch(cache, stuck_after)
    process = launch_process_tree(command, parent_lifetime=True)
    try:
        with terminate_unwinds():
            while True:
                try:
                    return process.wait(timeout=poll)
                except subprocess.TimeoutExpired:
                    pass
                stuck = watch.stuck()
                if stuck:
                    for path in stuck:
                        print(f"ccache lock {path} held for over {stuck_after:g} s: ccache waits on it "
                              "without end. Stopping the build; end the process holding it, or remove "
                              "the file if none does.", file=sys.stderr, flush=True)
                    return EXIT_STUCK
    finally:
        stop_process_tree(process)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stuck-after", type=float, default=STUCK_SECONDS,
                        help="seconds one lock may stay held (default %(default)s)")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no command to run")
    command[0] = shutil.which(command[0]) or command[0]
    return run(command, args.stuck_after)


if __name__ == "__main__":
    sys.exit(main())
