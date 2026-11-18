"""Run a build and stop it when a ccache lock stays held: ccache itself waits on a held lock forever.

    python ccache_watch.py [--stuck-after SECONDS] -- command [args...]

ccache has no lock timeout: a compile whose lock another process keeps retries every few
milliseconds without end, so the build hangs with no compiler running. ccache holds each lock
only for the milliseconds after a compile, so one that stays held, the same file, for
STUCK_SECONDS while no compile finishes is stuck. This stops the command's process tree and
fails naming the lock and, where the system can tell, its holder.

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
from process_tree import run_process_tree, terminate_unwinds  # noqa: E402

# ccache's log shows each lock held for milliseconds, after the compile.
STUCK_SECONDS = 60
EXIT_STUCK = 3
# The locks a compile waits on in ccache 4.x: lock/subdir_XX.lock and stats.lock on cache levels 1 and 2.
# auto_cleanup.lock is only ever tried, so a cleanup holding it long keeps no compile waiting.
LOCK_PATTERNS = ("lock/subdir_*.lock", "*/stats.lock", "*/*/stats.lock")
# Each compile, hit or miss, rewrites a stats file: one changing means the build still moves.
STATS_PATTERNS = ("*/stats", "*/*/stats")
# POSIX ccache refreshes a held long-lived lock's <name>.alive and breaks a lock idle for 2 s.
POSIX_ALIVE_SECONDS = 10
# An open lock refuses an exclusive open; a read-only one refuses write access; ccache retries both.
ERROR_ACCESS_DENIED = 5
ERROR_SHARING_VIOLATION = 32
CCACHE_QUERY_SECONDS = 30
# Holders Restart Manager reports at most; a stuck build has one or two.
RM_PROCESS_SLOTS = 16
# The stuck locks whose holders are asked for: Restart Manager takes seconds over hundreds of files.
HOLDER_QUERY_LOCKS = 4


def cache_dir():
    """The ccache cache directory this environment's builds use, or None without ccache."""
    ccache = shutil.which("ccache")
    if not ccache:
        return None
    found = subprocess.run([ccache, "--get-config", "cache_dir"], capture_output=True, text=True,
                           timeout=CCACHE_QUERY_SECONDS)
    return Path(found.stdout.strip()) if found.returncode == 0 and found.stdout.strip() else None


def windows_held(path):
    """Whether ccache would wait on `path`: its own exclusive open fails because another process has the file
    open, or because it refuses write access. The probe holds it exclusively for an instant, without ccache's
    delete-on-close; a ccache trying meanwhile retries."""
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    read_write_delete, share_none, open_existing = 0x80000000 | 0x40000000 | 0x10000, 0, 3
    handle = kernel.CreateFileW(str(path), read_write_delete, share_none, None, open_existing, 0, None)
    if handle == wintypes.HANDLE(-1).value:
        return ctypes.get_last_error() in (ERROR_SHARING_VIOLATION, ERROR_ACCESS_DENIED)
    kernel.CloseHandle(handle)
    return False


def posix_held(path):
    """Whether `path`'s holder still refreshes its keepalive; any other lock a waiter breaks itself."""
    try:
        return time.time() - path.with_suffix(".alive").stat().st_mtime < POSIX_ALIVE_SECONDS
    except OSError:
        return False


def is_held(path):
    return windows_held(path) if os.name == "nt" else posix_held(path)


def windows_holders(paths):
    """The processes holding any of `paths`, as one Restart Manager session reports them."""
    import ctypes
    from ctypes import wintypes

    class UniqueProcess(ctypes.Structure):
        _fields_ = [("pid", wintypes.DWORD), ("started", wintypes.FILETIME)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [("process", UniqueProcess), ("name", wintypes.WCHAR * 256),
                    ("service", wintypes.WCHAR * 64), ("type", ctypes.c_int), ("status", wintypes.DWORD),
                    ("session", wintypes.DWORD), ("restartable", wintypes.BOOL)]

    rm = ctypes.WinDLL("rstrtmgr")
    session, key = wintypes.DWORD(), ctypes.create_unicode_buffer(64)
    if rm.RmStartSession(ctypes.byref(session), 0, key):
        return []
    try:
        files = (wintypes.LPCWSTR * len(paths))(*map(str, paths))
        if rm.RmRegisterResources(session, len(paths), files, 0, None, 0, None):
            return []
        needed, count, reasons = wintypes.UINT(), wintypes.UINT(RM_PROCESS_SLOTS), wintypes.DWORD()
        found = (ProcessInfo * RM_PROCESS_SLOTS)()
        if rm.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), found, ctypes.byref(reasons)):
            return []
        return [f"pid {info.process.pid} ({info.name})" for info in found[:count.value]]
    finally:
        rm.RmEndSession(session)


def holders(paths):
    """Who holds the locks, as far as the system tells: Windows' Restart Manager, or each POSIX lock's own
    content, which ccache writes as its host and pid."""
    if os.name == "nt":
        return windows_holders(paths)
    named = []
    for path in paths:
        try:
            named.append(os.readlink(path))
        except OSError:
            pass
    return sorted(set(named))


def identities(cache, patterns):
    """Each file under `cache` matching `patterns`, mapped to an identity that changes when it is made anew."""
    found = {}
    for pattern in patterns:
        for path in cache.glob(pattern):
            try:
                status = path.lstat()
            except OSError:
                continue
            found[path] = (status.st_ino, status.st_ctime_ns, status.st_mtime_ns)
    return found


class StuckLocks(Exception):
    """Raised by a LockWatch with the locks held past its limit and their holders, named while the build,
    which may hold them itself, still runs."""


class LockWatch:
    """A call raises StuckLocks once locks have been the same held files for `stuck_after` seconds and no
    compile has finished meanwhile."""

    def __init__(self, cache, stuck_after=STUCK_SECONDS):
        self.cache, self.stuck_after = cache, stuck_after
        self.first_seen = {}
        self.stats, self.moved_at = None, None

    def stuck(self):
        now = time.monotonic()
        stats = identities(self.cache, STATS_PATTERNS)
        if stats != self.stats:
            self.stats, self.moved_at = stats, now
        self.first_seen = {lock: self.first_seen.get(lock, now)
                           for lock in identities(self.cache, LOCK_PATTERNS).items()}
        if now - self.moved_at < self.stuck_after:
            return []
        return sorted(path for (path, _), since in self.first_seen.items()
                      if now - since >= self.stuck_after and is_held(path))

    def __call__(self):
        stuck = self.stuck()
        if stuck:
            raise StuckLocks(stuck, holders(stuck[:HOLDER_QUERY_LOCKS]))


def run(command, stuck_after=STUCK_SECONDS):
    """The command's exit status, or EXIT_STUCK after stopping it on a stuck ccache lock."""
    cache = cache_dir()
    if cache is None:
        return subprocess.run(command).returncode
    try:
        with terminate_unwinds():
            return run_process_tree(command, watch=LockWatch(cache, stuck_after), parent_lifetime=True).returncode
    except StuckLocks as stuck:
        paths, held_by = stuck.args
        for path in paths:
            print(f"ccache lock {path} held for over {stuck_after:g} s with no compile finishing: ccache "
                  "waits on it without end.", file=sys.stderr)
        print(f"Stopped the build. Held by {', '.join(held_by)}: if that is not gone with the build, end it."
              if held_by else "Stopped the build. No process named as the holder: remove the lock file.",
              file=sys.stderr, flush=True)
        return EXIT_STUCK


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
    if os.name == "nt" and command[0].lower().endswith(".py"):
        # Windows starts no .py file itself; ESP-IDF's idf.py.exe is absent from some installations.
        # POSIX runs it by its own #! line.
        command.insert(0, sys.executable)
    return run(command, args.stuck_after)


if __name__ == "__main__":
    sys.exit(main())
