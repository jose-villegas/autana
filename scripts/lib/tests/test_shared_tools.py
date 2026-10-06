"""Shared pinned tool resolution and native path conversion contracts."""
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pinned_tool
import native_path


class SharedToolsTests(unittest.TestCase):
    def test_pinned_candidate_wins_over_earlier_wrong_major(self):
        with mock.patch.object(pinned_tool, 'candidates', return_value=['old', 'pinned']), \
             mock.patch.object(pinned_tool, 'tool_major', side_effect=['20', '19']):
            self.assertEqual(pinned_tool.resolve('clang-tidy'), ('pinned', '19'))

    def test_wrong_major_is_rejected_without_escape_hatch(self):
        with mock.patch.dict(os.environ, {}, clear=True), \
             mock.patch.object(pinned_tool, 'candidates', return_value=['old']), \
             mock.patch.object(pinned_tool, 'tool_major', return_value='20'):
            with self.assertRaises(SystemExit):
                pinned_tool.resolve('clang-format')

    def test_escape_hatch_uses_first_working_fallback(self):
        with mock.patch.dict(os.environ, {'CLANG_TIDY_ANY_VERSION': '1'}), \
             mock.patch.object(pinned_tool, 'candidates', return_value=['old']), \
             mock.patch.object(pinned_tool, 'tool_major', return_value='18'):
            self.assertEqual(pinned_tool.resolve('clang-tidy'), ('old', '18'))

    def test_native_conversion_on_windows_and_linux(self):
        with mock.patch.object(native_path.os, 'name', 'nt'):
            self.assertEqual(native_path.to_native('/c/space here/a.c'), 'c:/space here/a.c')
            self.assertEqual(native_path.to_native('--check'), '--check')
        with mock.patch.object(native_path.os, 'name', 'posix'):
            self.assertEqual(native_path.to_native('/c/a.c'), '/c/a.c')
