"""Process tree ownership and Windows job failures."""
import contextlib
import io
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import process_tree


class ProcessTreeTests(unittest.TestCase):
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
