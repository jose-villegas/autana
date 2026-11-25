"""Memory admission and ordered process results."""
import pathlib
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.process_budget import worker_capacity

class StatusTests(unittest.TestCase):
    def test_status_fields_and_missing_process(self):
        from unittest.mock import patch
        from r3d import process_budget as budget
        with patch.object(pathlib.Path, 'read_text', return_value="VmRSS: 12 kB\nVmHWM: 34 kB\n") as read:
            self.assertEqual(budget.status_bytes(123, "VmRSS"), 12 * 1024)
            self.assertEqual(budget.resident_bytes(123, {123: 56}), (12 * 1024, 56, 12 * 1024))
            self.assertEqual(budget.peak_rss(123), 34 * 1024)
            self.assertEqual(budget.status_bytes(123, "absent"), 0)
        with patch.object(pathlib.Path, 'read_text', side_effect=FileNotFoundError):
            self.assertEqual(budget.status_bytes(123, "VmRSS"), 0)


class CapacityTests(unittest.TestCase):
    def test_limiting_resource(self):
        self.assertEqual(worker_capacity((100, 60, 90), (20, 10, 30), (20, 20, 0), 10), 3)
    def test_floor_is_preserved(self):
        self.assertEqual(worker_capacity((19, 100), (1, 10), (20, 20), 10), 0)
    def test_cpu_limit(self):
        self.assertEqual(worker_capacity((1000,), (10,), (0,), 3), 3)
    def test_unused_resource(self):
        self.assertEqual(worker_capacity((100, 0), (10, 0), (20, 0), 20), 8)
    def test_invalid_estimate(self):
        with self.assertRaises(ValueError):
            worker_capacity((100,), (-1,), (0,), 2)


def fail_worker():
    raise RuntimeError("worker sentinel failure")


# Longer than any test waits: a worker holding a thread this long never exits on its own.
LINGER_SECONDS = 600


def lingering_worker():
    """Answers, then cannot exit: a non-daemon thread outlives the task."""
    import threading
    import time
    threading.Thread(target=time.sleep, args=(LINGER_SECONDS,)).start()
    return "answered"


class ProjectionTests(unittest.TestCase):
    def test_running_allocations_are_not_counted_twice(self):
        from r3d.process_budget import projected_available
        self.assertEqual(projected_available((100, 100, 100),
                         [((80, 40, 80), (60, 30, 60))]), (80, 90, 80))

    def test_not_started_reserves_full_estimate(self):
        from r3d.process_budget import projected_available
        self.assertEqual(projected_available((100,) * 3, [((80,) * 3, (0,) * 3)]), (20, 20, 20))

    def test_over_estimate_does_not_invent_available_memory(self):
        from r3d.process_budget import projected_available
        self.assertEqual(projected_available((100,) * 3, [((20,) * 3, (80,) * 3)]), (100,) * 3)


def identity_worker(value):
    import os
    return value, os.getpid()


class CleanupTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'linux', 'Linux parent death signal')
    def test_worker_dies_when_parent_is_killed(self):
        import os
        import subprocess
        import tempfile
        import time
        with tempfile.TemporaryDirectory() as directory:
            marker = pathlib.Path(directory) / 'pid'
            code = """
import multiprocessing, pathlib, sys, time
sys.path.insert(0, sys.argv[1])
from r3d.process_budget import _task

def work():
    import os
    pathlib.Path(sys.argv[2]).write_text(str(os.getpid()))
    time.sleep(60)

if __name__ == '__main__':
    context = multiprocessing.get_context('spawn')
    receive, send = context.Pipe(False)
    process = context.Process(target=_task, args=(send, work, (), (0,) * 3))
    process.start()
    time.sleep(60)
"""
            script = pathlib.Path(directory) / 'parent.py'
            script.write_text(code)
            parent = subprocess.Popen([sys.executable, str(script), str(pathlib.Path(__file__).resolve().parents[1]), str(marker)])
            try:
                deadline = time.monotonic() + 5
                while not marker.exists() and time.monotonic() < deadline:
                    time.sleep(.05)
                self.assertTrue(marker.exists())
                pid = int(marker.read_text())
                parent.kill()
                parent.wait(timeout=5)
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    try:
                        state = pathlib.Path(f'/proc/{pid}/status').read_text()
                    except (FileNotFoundError, ProcessLookupError):
                        break
                    if 'State:\tZ' in state:
                        break
                    time.sleep(.05)
                else:
                    self.fail('worker survived parent SIGKILL')
            finally:
                if parent.poll() is None:
                    parent.kill()
                parent.wait(timeout=5)
                if marker.exists():
                    try:
                        os.kill(int(marker.read_text()), 9)
                    except ProcessLookupError:
                        pass


def reservation_worker():
    from r3d.process_budget import task_reservation
    return task_reservation()


def handshake_worker(started, release, value, ended=None):
    import os
    started.set()
    try:
        if not release.wait(20):
            raise RuntimeError('handshake timed out')
        return value, os.getpid()
    finally:
        if ended is not None:
            ended.set()


def exit_worker():
    import os
    os._exit(7)


class MeasuredBudgetTests(unittest.TestCase):
    def test_worker_estimates(self):
        from r3d import process_budget as b
        self.assertEqual(b.PREPARE_BYTES, (21 * b.GIB // 4, 0, 21 * b.GIB // 4))
        self.assertEqual(b.BAKE_BYTES, (17 * b.GIB // 4, 0, 17 * b.GIB // 4))
        self.assertEqual(b.FIT_BYTES, (22 * b.GIB // 10, 7 * b.GIB // 10, 22 * b.GIB // 10))
        self.assertEqual(b.MEASURE_BYTES, (3 * b.GIB // 2, 128 * 1024 ** 2, 3 * b.GIB // 2))
        self.assertEqual(b.SMOKE_PREPARE_BYTES, (2 * b.GIB, 0, 2 * b.GIB))

    def test_pss_reads_rollup_and_handles_unavailable_metric(self):
        from unittest.mock import patch
        from r3d import process_budget as b
        with patch.object(pathlib.Path, 'read_text', return_value='Rss: 999 kB\nPss: 123 kB\n') as read:
            self.assertEqual(b.pss_bytes(42), 123 * 1024)
            self.assertEqual(read.call_args.args, ())
        for error in (FileNotFoundError, PermissionError):
            with patch.object(pathlib.Path, 'read_text', side_effect=error):
                self.assertEqual(b.pss_bytes(42), 0)
        with patch.object(pathlib.Path, 'read_text', return_value='Rss: 999 kB\n'):
            self.assertEqual(b.pss_bytes(42), 0)
