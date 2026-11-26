"""ccache_watch.py against a real ccache: a lock another process holds stops the build, naming the lock,
and leaves no ccache behind; a free cache, an unheld lock file and the command's own status pass through."""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
from pathlib import Path

BUILD = Path(__file__).resolve().parents[1] / "build"
sys.path.insert(0, str(BUILD))
import ccache_watch  # noqa: E402

WATCH = BUILD / "ccache_watch.py"
HEX = "0123456789abcdef"
# Every lock ccache takes around a cache miss: one of these per result subdirectory.
SUBDIR_LOCKS = [f"lock/subdir_{a}{b}.lock" for a in HEX for b in HEX]
STUCK_AFTER = 3
# Generous for a slow runner, and a twentieth of a hang's wait.
RUN_TIMEOUT = 60
HELD = "held"
UNTIL_KILLED = "until-killed"


def pinned_ccache():
    """The ccache the build uses: ESP-IDF's pinned one, else the one on PATH."""
    try:
        import espressif
        name = "ccache.exe" if os.name == "nt" else "ccache"
        for found in sorted(espressif.espressif_tools_root().glob(f"tools/ccache/*/*/{name}")):
            return found
    except Exception:
        pass
    found = shutil.which("ccache")
    return Path(found) if found else None


CCACHE = pinned_ccache()
def real_compiler():
    """The first gcc or cc on PATH that is not a ccache under a compiler's name (its masquerade)."""
    caches = {Path(found).resolve().read_bytes() for found in (CCACHE, shutil.which("ccache")) if found}
    for name in ("gcc", "cc"):
        for folder in os.environ.get("PATH", "").split(os.pathsep):
            found = shutil.which(name, path=folder)
            if found and Path(found).resolve().read_bytes() not in caches:
                return found
    return None


CC = real_compiler()


def hold(cache, paths, keepalive, until_killed=False):
    """Hold each lock as ccache does until stdin closes, or until killed: Windows opens it sharing nothing;
    POSIX makes the symlink and keeps its .alive fresh."""
    files = []
    for name in paths:
        path = Path(cache) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateFileW.restype = wintypes.HANDLE
            open_always, delete_on_close = 4, 0x04000000
            handle = kernel.CreateFileW(str(path), 0xC0000000, 0, None, open_always, delete_on_close, None)
            assert handle != wintypes.HANDLE(-1).value, ctypes.get_last_error()
            files.append(handle)
        else:
            os.symlink(f"holder-{os.getpid()}", path)
            files.append(path.with_suffix(".alive"))
    print(HELD, flush=True)
    if os.name != "nt" and keepalive:
        import threading
        threading.Thread(target=refresh, args=(files,), daemon=True).start()
    if until_killed:
        while True:
            time.sleep(1)
    sys.stdin.read()


def refresh(alive):
    while True:
        for path in alive:
            path.touch()
        time.sleep(0.5)


@unittest.skipUnless((CCACHE and CC) or os.environ.get("CI"), "needs ccache and a C compiler")
class CcacheWatchTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(CCACHE and CC, "CI runs this suite with ccache and a C compiler installed")
        self.root = Path(tempfile.mkdtemp(prefix="ccache watch "))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.cache = self.root / "cache"
        (self.root / "a.c").write_text("int answer(void) { return 42; }\n")
        self.env = dict(os.environ, CCACHE_DIR=str(self.cache),
                        PATH=str(CCACHE.parent) + os.pathsep + os.environ["PATH"])

    def holder(self, paths, keepalive=True):
        process = subprocess.Popen([sys.executable, __file__, str(self.cache), str(int(keepalive)), *paths],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(process.stdout.close)
        self.addCleanup(process.wait, 10)
        self.addCleanup(process.stdin.close)
        self.assertEqual(process.stdout.readline().strip(), HELD)
        return process

    def results(self):
        """What ccache stored, under the lock: result and manifest files two levels below the cache, renamed
        from the .tmp files it writes before taking the lock."""
        return [path for path in self.cache.glob("?/?/*")
                if path.is_file() and path.name != "stats" and not path.name.endswith(".tmp")]

    def watch(self, *command):
        return subprocess.run([sys.executable, str(WATCH), "--stuck-after", str(STUCK_AFTER), "--", *command],
                              cwd=self.root, env=self.env, capture_output=True, text=True, timeout=RUN_TIMEOUT,
                              stdin=subprocess.DEVNULL)

    def compile(self):
        return self.watch("ccache", CC, "-c", "a.c", "-o", "a.o")

    def test_held_lock_stops_build_naming_it(self):
        holder = self.holder(SUBDIR_LOCKS)
        started = time.monotonic()
        result = self.compile()
        self.assertEqual(result.returncode, ccache_watch.EXIT_STUCK, result.stderr)
        self.assertLess(time.monotonic() - started, RUN_TIMEOUT / 2)
        named = [line for line in result.stderr.splitlines() if line.startswith("ccache lock ")]
        self.assertTrue(named, result.stderr)
        self.assertIn(str(self.cache / "lock" / "subdir_"), named[0].replace("/", os.sep))
        self.assertIn(f"pid {holder.pid}" if os.name == "nt" else f"holder-{holder.pid}", result.stderr)
        # A ccache the watch left running would store its result once the lock is free.
        holder.stdin.close()
        holder.wait(10)
        time.sleep(STUCK_AFTER)
        self.assertEqual(self.results(), [], "a ccache outlived the stopped build")

    def test_holder_inside_the_build_is_named(self):
        # The build itself holds the lock: named before the stop ends it.
        result = self.watch(sys.executable, __file__, str(self.cache), UNTIL_KILLED, SUBDIR_LOCKS[0])
        self.assertEqual(result.returncode, ccache_watch.EXIT_STUCK, result.stderr)
        self.assertIn("Held by " + ("pid " if os.name == "nt" else "holder-"), result.stderr)

    def test_free_cache_builds(self):
        result = self.compile()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / "a.o").exists())
        self.assertTrue(self.results())

    def test_unheld_lock_file_is_not_stuck(self):
        # Windows: a file no process has open; POSIX: a lock whose keepalive stopped, which ccache breaks.
        holder = self.holder(SUBDIR_LOCKS[:1], keepalive=False)
        if os.name == "nt":
            holder.stdin.close()
            holder.wait(10)
            (self.cache / SUBDIR_LOCKS[0]).write_text("")
        result = self.watch(sys.executable, "-c", f"import time; time.sleep({STUCK_AFTER * 2})")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_command_status_passes_through(self):
        result = self.watch(sys.executable, "-c", "raise SystemExit(7)")
        self.assertEqual(result.returncode, 7, result.stderr)

    def test_every_compile_changes_the_stats_the_watch_reads(self):
        # A miss and then a hit each rewrite a stats file: the progress the watch waits for.
        for outcome in ("miss", "hit"):
            before = ccache_watch.identities(self.cache, ccache_watch.STATS_PATTERNS)
            time.sleep(0.05)
            result = subprocess.run(["ccache", CC, "-c", "a.c", "-o", "a.o"], cwd=self.root, env=self.env,
                                    capture_output=True, text=True, timeout=RUN_TIMEOUT)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotEqual(ccache_watch.identities(self.cache, ccache_watch.STATS_PATTERNS), before, outcome)

    def test_held_cleanup_lock_keeps_no_compile_waiting(self):
        # Why the watch leaves auto_cleanup.lock alone.
        self.holder(["lock/auto_cleanup.lock"])
        result = subprocess.run(["ccache", CC, "-c", "a.c", "-o", "a.o"], cwd=self.root, env=self.env,
                                capture_output=True, text=True, timeout=RUN_TIMEOUT)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_held_lock_hangs_ccache_unwatched(self):
        # The premise: without the watch, ccache waits on the held lock.
        self.holder(SUBDIR_LOCKS)
        compile = subprocess.Popen(["ccache", CC, "-c", "a.c", "-o", "a.o"], cwd=self.root, env=self.env)
        try:
            with self.assertRaises(subprocess.TimeoutExpired):
                compile.wait(STUCK_AFTER)
        finally:
            compile.kill()
            compile.wait()


class LockWatchTests(unittest.TestCase):
    """The rules on a cache directory alone, with the clock and the held probe in the test's hands."""

    def setUp(self):
        self.cache = Path(tempfile.mkdtemp(prefix="ccache watch "))
        self.addCleanup(shutil.rmtree, self.cache, ignore_errors=True)
        self.now = 0.0
        clock = mock.patch.object(ccache_watch.time, "monotonic", lambda: self.now)
        held = mock.patch.object(ccache_watch, "is_held", lambda path: True)
        clock.start()
        held.start()
        self.addCleanup(clock.stop)
        self.addCleanup(held.stop)

    def lock(self, name):
        path = self.cache / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("")
        return path

    def test_each_lock_a_compile_waits_on_is_watched(self):
        names = ["lock/subdir_3f.lock", "7/stats.lock", "7/5/stats.lock"]
        for name in names:
            self.lock(name)
        # ccache only tries the cleanup lock, so a long cleanup keeps no compile waiting.
        self.lock("lock/auto_cleanup.lock")
        self.lock("7/5/result.tmp")
        watch = ccache_watch.LockWatch(self.cache, STUCK_AFTER)
        self.assertEqual(watch.stuck(), [])
        self.now += STUCK_AFTER
        self.assertEqual(watch.stuck(), sorted(self.cache / name for name in names))

    def test_lock_taken_anew_restarts_its_clock(self):
        # Many short holds of one lock are a busy build, not a stuck one.
        path = self.lock("lock/subdir_00.lock")
        watch = ccache_watch.LockWatch(self.cache, STUCK_AFTER)
        watch.stuck()
        for _ in range(3):
            self.now += STUCK_AFTER / 2
            path.unlink()
            time.sleep(0.05)
            self.lock("lock/subdir_00.lock")
            self.assertEqual(watch.stuck(), [])
        self.now += STUCK_AFTER
        self.assertEqual(watch.stuck(), [path])

    def test_finishing_compiles_keep_a_held_lock_from_stopping_the_build(self):
        # Another process's long hold on one subdirectory while this build's compiles still finish.
        path = self.lock("lock/subdir_00.lock")
        stats = self.lock("5/c/stats")
        watch = ccache_watch.LockWatch(self.cache, STUCK_AFTER)
        watch.stuck()
        for step in range(1, 4):
            self.now += STUCK_AFTER / 2
            os.utime(stats, ns=(step * 10**9, step * 10**9))
            self.assertEqual(watch.stuck(), [])
        self.now += STUCK_AFTER
        self.assertEqual(watch.stuck(), [path])

    @unittest.skipUnless(os.name == "nt", "Windows open semantics")
    def test_windows_read_only_lock_is_held(self):
        # ccache cannot open it for writing and retries without end.
        path = self.lock("lock/subdir_00.lock")
        self.assertFalse(ccache_watch.windows_held(path))
        path.chmod(0o444)
        self.addCleanup(path.chmod, 0o666)
        self.assertTrue(ccache_watch.windows_held(path))

    @unittest.skipUnless(os.name == "nt", "Windows open semantics")
    def test_windows_lock_open_elsewhere_is_held(self):
        # Even a handle that shares everything makes ccache's exclusive open fail.
        import ctypes
        from ctypes import wintypes
        path = self.lock("lock/subdir_00.lock")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.restype = wintypes.HANDLE
        generic_read, share_all, open_existing = 0x80000000, 0x7, 3
        handle = kernel.CreateFileW(str(path), generic_read, share_all, None, open_existing, 0, None)
        self.assertNotEqual(handle, wintypes.HANDLE(-1).value)
        try:
            self.assertTrue(ccache_watch.windows_held(path))
        finally:
            kernel.CloseHandle(handle)
        self.assertFalse(ccache_watch.windows_held(path))

    def test_unheld_lock_is_not_stuck(self):
        self.lock("lock/subdir_00.lock")
        watch = ccache_watch.LockWatch(self.cache, STUCK_AFTER)
        watch.stuck()
        self.now += STUCK_AFTER
        with mock.patch.object(ccache_watch, "is_held", lambda path: False):
            self.assertEqual(watch.stuck(), [])

    @unittest.skipIf(os.name == "nt", "POSIX keepalive")
    def test_posix_lock_is_held_while_its_keepalive_is_fresh(self):
        path = self.lock("lock/subdir_00.lock")
        alive = path.with_suffix(".alive")
        self.assertFalse(ccache_watch.posix_held(path))
        alive.write_text("")
        fresh = time.time() - ccache_watch.POSIX_ALIVE_SECONDS + 1
        os.utime(alive, (fresh, fresh))
        self.assertTrue(ccache_watch.posix_held(path))
        stale = time.time() - ccache_watch.POSIX_ALIVE_SECONDS - 1
        os.utime(alive, (stale, stale))
        self.assertFalse(ccache_watch.posix_held(path))


class CommandTests(unittest.TestCase):
    """A .py command starts as the platform starts it: Windows through the interpreter, POSIX by its #! line,
    which for ESP-IDF's stand-ins may be a shell."""

    def test_python_script_runs_where_the_platform_runs_it(self):
        with tempfile.TemporaryDirectory() as scratch:
            script = Path(scratch) / "tool.py"
            script.write_text("raise SystemExit(5)\n" if os.name == "nt" else "#!/bin/sh\nexit 5\n")
            script.chmod(0o755)
            with mock.patch.object(ccache_watch, "cache_dir", lambda: None):
                self.assertEqual(ccache_watch.main([str(script)]), 5)


class UnwatchedTests(unittest.TestCase):
    def test_without_ccache_the_command_runs_unwatched(self):
        with mock.patch.object(ccache_watch.shutil, "which", lambda name: None):
            self.assertEqual(ccache_watch.run([sys.executable, "-c", "raise SystemExit(7)"]), 7)


if __name__ == "__main__":
    hold(sys.argv[1], sys.argv[3:], sys.argv[2] != "0", until_killed=sys.argv[2] == UNTIL_KILLED)
