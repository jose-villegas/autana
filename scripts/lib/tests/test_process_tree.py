"""Process tree ownership and Windows job failures."""
import contextlib
import io
import os
from pathlib import Path
import sys
import subprocess
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
