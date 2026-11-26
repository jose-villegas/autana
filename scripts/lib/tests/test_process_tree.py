"""Process tree ownership and Windows job failures."""
import contextlib
import io
import os
import queue
from pathlib import Path
import sys
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import process_tree


class ProcessTreeTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux parent death signal")
    def test_parent_resolves_prctl_before_child_calls_it(self):
        import ctypes
        kernel = Mock()
        kernel.prctl.return_value = 0
        with patch.object(ctypes, 'CDLL', return_value=kernel) as loaded:
            bind = process_tree.bind_parent_lifetime(os.getpid())
            loaded.assert_called_once()
            kernel.prctl.assert_not_called()
        with patch.object(process_tree.os, 'getppid', return_value=os.getpid()):
            bind()
        kernel.prctl.assert_called_once_with(process_tree.PR_SET_PDEATHSIG,
                                             process_tree.signal.SIGKILL, 0, 0, 0)

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux parent death signal")
    def test_changed_parent_kills_child_before_command_runs(self):
        for parent_pid in (os.getpid(), os.getppid()):
            with self.subTest(parent_pid=parent_pid):
                child = subprocess.Popen([sys.executable, "-c", "print('command ran')"],
                                         preexec_fn=process_tree.bind_parent_lifetime(parent_pid),
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                self.addCleanup(lambda p=child: (p.kill(), p.wait()) if p.poll() is None else None)
                stdout, stderr = child.communicate(timeout=10)
                self.assertEqual(child.returncode, 0 if parent_pid == os.getpid()
                                 else -process_tree.signal.SIGKILL, stderr)
                self.assertEqual(stdout, "command ran\n" if parent_pid == os.getpid() else "")

    @unittest.skipUnless(os.name == 'nt', 'Windows job assignment')
    def test_job_failure_degrades_and_assignment_precedes_resume(self):
        import ctypes
        for created, assigned in ((None, False), (123, False), (123, True)):
            kernel, process, ntdll = Mock(), Mock(), Mock()
            process._handle = 456
            kernel.AssignProcessToJobObject.return_value = assigned
            ntdll.NtResumeProcess.return_value = 0
            order = []
            kernel.AssignProcessToJobObject.side_effect = lambda *args: order.append('assign') or assigned
            ntdll.NtResumeProcess.side_effect = lambda *args: order.append('resume') or 0
            with patch.object(process_tree, 'windows_job_binding', return_value=kernel), \
                    patch.object(process_tree, 'create_kill_on_close_job', return_value=created), \
                    patch.object(process_tree.subprocess, 'Popen', return_value=process) as popen, \
                    patch.object(ctypes, 'WinDLL', return_value=ntdll), contextlib.redirect_stderr(io.StringIO()):
                self.assertIs(process_tree.launch_process_tree(['command']), process)
                if created:
                    self.assertEqual(order, ['assign', 'resume'])
                    self.assertTrue(popen.call_args.kwargs['creationflags'] & process_tree.CREATE_SUSPENDED)
                    if assigned:
                        self.assertEqual(process._tree_job, 123)
                    else:
                        kernel.CloseHandle.assert_called_once_with(123)
                else:
                    self.assertNotIn('creationflags', popen.call_args.kwargs)

    @unittest.skipUnless(os.name == 'nt', 'Windows job creation')
    def test_shared_job_creation_failure_degrades(self):
        for created, configured in ((None, False), (123, False)):
            kernel = Mock()
            kernel.CreateJobObjectW.return_value = created
            kernel.SetInformationJobObject.return_value = configured
            with contextlib.redirect_stderr(io.StringIO()) as log:
                self.assertIsNone(process_tree.create_kill_on_close_job(kernel=kernel))
            self.assertEqual(log.getvalue(), '')
            if created:
                kernel.CloseHandle.assert_called_once_with(created)


# A grandchild that outlives its stopped tree touches its marker this long after it starts; the deadline leaves
# a loaded Windows runner time to start Python twice before it.
GRANDCHILD_SECONDS = 4
DEADLINE_SECONDS = 2


class RunProcessTreeTests(unittest.TestCase):
    """run_process_tree on real processes: a child that hangs after starting a grandchild holding its pipes."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.marker = Path(self.temp.name) / "grandchild-outlived-the-tree"

    def hung_child(self):
        grandchild = (f"import time; from pathlib import Path; time.sleep({GRANDCHILD_SECONDS}); "
                      f"Path({str(self.marker)!r}).touch()")
        child = (f"import subprocess, sys, time; subprocess.Popen([sys.executable, '-c', {grandchild!r}]); "
                 "print('started', flush=True); time.sleep(60)")
        return [sys.executable, "-c", child]

    def assert_tree_stopped(self, started):
        self.assertLess(time.monotonic() - started, GRANDCHILD_SECONDS)
        time.sleep(GRANDCHILD_SECONDS + 1 - (time.monotonic() - started))
        self.assertFalse(self.marker.exists(), "the grandchild outlived the stopped tree")

    def test_deadline_stops_child_and_grandchild_in_both_output_modes(self):
        for on_line in (None, Mock()):
            with self.subTest(on_line=on_line):
                started = time.monotonic()
                pipes = {} if on_line else dict(stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                with self.assertRaises(subprocess.TimeoutExpired) as late:
                    process_tree.run_process_tree(self.hung_child(), DEADLINE_SECONDS, on_line=on_line, **pipes)
                self.assert_tree_stopped(started)
                if on_line:
                    on_line.assert_called_once_with("started\n")
                else:
                    self.assertEqual(late.exception.output, "started\n")

    def test_watch_raising_stops_the_tree(self):
        calls = []

        def watch():
            calls.append(time.monotonic())
            if len(calls) == 2:
                raise LookupError("lock lost")
        started = time.monotonic()
        with self.assertRaisesRegex(LookupError, "lock lost"):
            process_tree.run_process_tree(self.hung_child(), 60, watch=watch, stdout=subprocess.DEVNULL)
        self.assertGreaterEqual(calls[1] - calls[0], process_tree.WATCH_SECONDS * 0.9)
        self.assert_tree_stopped(started)

    def test_on_line_failure_stops_the_tree(self):
        started = time.monotonic()
        with self.assertRaisesRegex(KeyboardInterrupt, "stop"):
            process_tree.run_process_tree(self.hung_child(), on_line=Mock(side_effect=KeyboardInterrupt("stop")))
        self.assert_tree_stopped(started)

    def test_a_stop_that_times_out_never_hides_the_error_that_stopped_the_run(self):
        stop = process_tree.stop_process_tree

        def timed_out_stop(process):
            stop(process)
            raise subprocess.TimeoutExpired("taskkill", 10)
        started = time.monotonic()
        with patch.object(process_tree, "stop_process_tree", side_effect=timed_out_stop), \
                self.assertRaisesRegex(KeyboardInterrupt, "stop"):
            process_tree.run_process_tree(self.hung_child(), on_line=Mock(side_effect=KeyboardInterrupt("stop")))
        self.assert_tree_stopped(started)

    def test_a_run_ends_with_its_output_so_a_grandchild_holding_the_pipe_meets_the_deadline(self):
        # The child exits at once; its grandchild keeps the output pipe, so the run is not over until the deadline
        # stops the grandchild too, and the line reader's pipe is closed once it has seen the end.
        grandchild = (f"import time; from pathlib import Path; time.sleep({GRANDCHILD_SECONDS}); "
                      f"Path({str(self.marker)!r}).touch()")
        child = (f"import subprocess, sys; subprocess.Popen([sys.executable, '-c', {grandchild!r}]); "
                 "print('done', flush=True); sys.exit(5)")
        launched = []
        launch = process_tree.launch_process_tree
        started = time.monotonic()
        with patch.object(process_tree, "launch_process_tree",
                          side_effect=lambda *args, **kwargs: launched.append(launch(*args, **kwargs)) or launched[-1]), \
                self.assertRaises(subprocess.TimeoutExpired):
            process_tree.run_process_tree([sys.executable, "-c", child], DEADLINE_SECONDS, on_line=Mock())
        self.assertEqual(launched[0].returncode, 5)
        self.assertTrue(launched[0].stdout.closed)
        self.assert_tree_stopped(started)

    def test_on_line_errors_of_the_wait_types_still_end_the_run(self):
        for error in (subprocess.TimeoutExpired("probe", 1), queue.Empty()):
            with self.subTest(error=type(error).__name__), self.assertRaises(type(error)):
                process_tree.run_process_tree(self.hung_child(), 30, on_line=Mock(side_effect=error))

    def test_an_undecodable_line_fails_the_run(self):
        # 0x81 is neither UTF-8 nor cp1252, the two text encodings these runs meet.
        command = [sys.executable, "-c", "import sys, time; sys.stdout.buffer.write(bytes([0x81, 10])); "
                   "sys.stdout.flush(); time.sleep(60)"]
        started = time.monotonic()
        with self.assertRaises(UnicodeDecodeError):
            process_tree.run_process_tree(command, 30, on_line=Mock())
        self.assertLess(time.monotonic() - started, DEADLINE_SECONDS)

    def test_on_line_owns_the_pipes(self):
        for pipe in ("stdin", "stdout", "stderr"):
            with self.subTest(pipe=pipe), patch.object(process_tree, "launch_process_tree") as launch, \
                    self.assertRaises(ValueError):
                process_tree.run_process_tree(["never"], on_line=Mock(), **{pipe: subprocess.PIPE})
            launch.assert_not_called()

    def test_a_spent_deadline_never_starts_waiting(self):
        command = [sys.executable, "-c", "import time; time.sleep(60)"]
        for timeout in (0, -1):
            started = time.monotonic()
            with self.subTest(timeout=timeout), self.assertRaises(subprocess.TimeoutExpired):
                process_tree.run_process_tree(command, timeout)
            self.assertLess(time.monotonic() - started, DEADLINE_SECONDS)

    def test_a_child_that_closes_its_output_is_still_waited_for_and_watched(self):
        quiet = ("import os, sys, time; print('closing', flush=True); os.close(1); os.close(2); "
                 "time.sleep({}); sys.exit(4)")
        watch = Mock()
        done = process_tree.run_process_tree([sys.executable, "-c", quiet.format(1)], 30, watch=watch, on_line=Mock())
        self.assertEqual(done.returncode, 4)
        self.assertGreater(watch.call_count, 1)
        with self.assertRaises(subprocess.TimeoutExpired):
            process_tree.run_process_tree([sys.executable, "-c", quiet.format(60)], DEADLINE_SECONDS, on_line=Mock())

    def test_a_finished_run_returns_its_output_and_failure_code(self):
        command = [sys.executable, "-c", "import sys; print('a', flush=True); print('b', file=sys.stderr, flush=True); "
                   "print('c', flush=True); sys.exit(3)"]
        done = process_tree.run_process_tree(command, 30, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual((done.args, done.returncode, done.stdout, done.stderr), (command, 3, "a\nc\n", "b\n"))
        seen = []
        on_line = lambda line: seen.append((line, threading.current_thread()))  # noqa: E731
        done = process_tree.run_process_tree(command, 30, on_line=on_line)
        self.assertEqual((done.returncode, done.stdout), (3, None))
        self.assertEqual(seen, [(line, threading.main_thread()) for line in ("a\n", "b\n", "c\n")])


if __name__ == "__main__":
    unittest.main()
