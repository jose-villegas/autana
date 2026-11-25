"""shell_job.fake_running: a recorded fake runs while its pid is alive and is still the process that
recorded it, so a pid the system handed to a later process does not count."""
import os
import subprocess
import sys
import unittest

from shell_job import fake_running, process_start


class FakeRunningTests(unittest.TestCase):
    def setUp(self):
        self.child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        self.addCleanup(self.child.wait)
        self.addCleanup(self.child.kill)
        self.started = process_start(self.child.pid)

    def test_the_process_that_recorded_the_call_is_running(self):
        self.assertTrue(fake_running(dict(pid=self.child.pid, started=self.started)))

    def test_a_later_process_with_the_same_pid_is_not(self):
        if self.started is None:
            self.skipTest("this system gives no process start")
        self.assertFalse(fake_running(dict(pid=self.child.pid, started=self.started - 100)))

    def test_an_exited_process_is_not(self):
        self.child.kill()
        self.child.wait()
        self.assertFalse(fake_running(dict(pid=self.child.pid, started=self.started)))

    def test_a_start_unknown_when_recorded_falls_back_to_the_pid_being_alive(self):
        self.assertTrue(fake_running(dict(pid=self.child.pid, started=None)))

    def test_this_process_records_a_start(self):
        self.assertIsNotNone(process_start(os.getpid()))
