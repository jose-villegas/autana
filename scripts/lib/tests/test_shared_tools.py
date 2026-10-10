"""Shared tool resolution, shell paths and capture contracts."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

LIB = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LIB))
sys.path.insert(0, str(LIB.parent / 'device'))
import pinned_tool
import native_path
sys.path.insert(0, str(LIB.parent / 'device' / 'tests'))
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)
import device
import device_capture as capture


class SharedToolsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.versions = {}
        self.env = mock.patch.dict(os.environ, {'PATH': str(self.bin),
            'IDF_TOOLS_PATH': str(self.root), 'CLANG_TIDY': '',
            'CLANG_FORMAT_ANY_VERSION': '', 'CLANG_TIDY_ANY_VERSION': ''})
        self.env.start()
        self.addCleanup(self.env.stop)

    def candidate(self, name, major='19', version=None):
        folder = self.bin if version is None else self.root / 'tools/esp-clang' / version / 'esp-clang/bin'
        folder.mkdir(parents=True, exist_ok=True)
        suffix = '.exe' if os.name == 'nt' else ''
        path = folder / (name + suffix)
        path.write_text('fixture')
        path.chmod(0o755)
        self.versions[os.path.normcase(str(path))] = major
        return shutil.which(name) if version is None else str(path)

    def version_run(self, args, **kwargs):
        major = self.versions[os.path.normcase(shutil.which(args[0]) or args[0])]
        if major == 'timeout':
            raise subprocess.TimeoutExpired(args, 15)
        return subprocess.CompletedProcess(args, 1 if major == 'nonzero' else 0,
            'clang version 19' if major == 'nonzero' else 'clang version ' + major, '')

    def test_real_candidate_order_and_pinned_resolution_for_both_tools(self):
        for tool in ('clang-format', 'clang-tidy'):
            with self.subTest(tool=tool):
                versioned = self.candidate(tool + '-19', '20')
                plain = self.candidate(tool, '18')
                old = self.candidate(tool, version='v9')
                new = self.candidate(tool, version='v10')
                self.assertEqual(pinned_tool.candidates(tool), [versioned, plain, new, old])
                with mock.patch.object(subprocess, 'run', side_effect=self.version_run):
                    self.assertEqual(pinned_tool.resolve(tool), (new, '19'))
                    self.versions[os.path.normcase(versioned)] = '19'
                    self.assertEqual(pinned_tool.resolve(tool), (versioned, '19'))
                    self.versions[os.path.normcase(versioned)] = 'unparseable'
                    self.versions[os.path.normcase(plain)] = '19'
                    self.assertEqual(pinned_tool.resolve(tool), (plain, '19'))

    def test_override_and_first_wrong_major_fallback(self):
        override = self.candidate('custom-tidy', '18')
        self.candidate('clang-tidy-19', '20')
        with mock.patch.dict(os.environ, {'CLANG_TIDY': override}), \
             mock.patch.object(subprocess, 'run', side_effect=self.version_run):
            with self.assertRaises(SystemExit):
                pinned_tool.resolve('clang-tidy')
            with mock.patch.dict(os.environ, {'CLANG_TIDY_ANY_VERSION': '1'}):
                self.assertEqual(pinned_tool.resolve('clang-tidy'), (override, '18'))

    def test_missing_and_unusable_candidates_fail_for_both_tools(self):
        for tool in ('clang-format', 'clang-tidy'):
            with self.subTest(tool=tool), mock.patch.object(subprocess, 'run', side_effect=self.version_run):
                with self.assertRaises(SystemExit):
                    pinned_tool.resolve(tool)
                for version in ('unparseable', 'nonzero', 'timeout'):
                    with self.subTest(version=version):
                        self.candidate(tool + '-19', version)
                        with self.assertRaises(SystemExit):
                            pinned_tool.resolve(tool)
                fallback = self.candidate(tool, '18')
                with mock.patch.dict(os.environ, {tool.upper().replace('-', '_') + '_ANY_VERSION': '1'}):
                    self.assertEqual(pinned_tool.resolve(tool), (fallback, '18'))
                    first = self.candidate(tool + '-19', '20')
                    self.assertEqual(pinned_tool.resolve(tool), (first, '20'))

    def test_native_conversion_on_windows_and_linux(self):
        with mock.patch.object(native_path.os, 'name', 'nt'):
            self.assertEqual(native_path.to_native('/c/space here/a.c'), 'c:/space here/a.c')
            self.assertEqual(native_path.to_native('--check'), '--check')
        with mock.patch.object(native_path.os, 'name', 'posix'):
            self.assertEqual(native_path.to_native('/c/a.c'), '/c/a.c')


class ShellPathTests(unittest.TestCase):
    def test_shell_passthrough_cygpath_stdin_and_failure(self):
        shell = device.git_bash() if os.name == 'nt' else shutil.which('sh')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            prefix = 'PATH=$(cygpath -u "$1"); ' if os.name == 'nt' else 'PATH="$1"; '
            script = prefix + '. "' + (LIB / 'native_path.sh').as_posix() + '"; '
            env = dict(os.environ, PATH=root.as_posix())
            def run(code):
                return subprocess.run([shell, '-c', script + code, 'paths', root.as_posix()], env=env,
                    capture_output=True, text=True, timeout=30)
            result = run('to_native "/c/space here/a.c"')
            self.assertEqual((result.returncode, result.stdout), (0, '/c/space here/a.c\n'))
            cat = root / 'cat'
            cat.write_text('#!/bin/sh\nwhile IFS= read -r line; do printf \"%s\\n\" \"$line\"; done\n')
            cat.chmod(0o755)
            self.assertEqual(run('printf \"a\\nb\\n\" | to_native').stdout, 'a\nb\n')
            fake = root / 'cygpath'
            fake.write_text('#!/bin/sh\n[ "$1" = -m ] || exit 9\nshift\nif [ "$1" = -f ]; then while IFS= read -r line; do printf "native:%s\\n" "$line"; done; else printf "native:%s\\n" "$1"; fi\n')
            fake.chmod(0o755)
            self.assertEqual(run('to_native "/c/space here/a.c"').stdout, 'native:/c/space here/a.c\n')
            self.assertEqual(run('printf "a\\nb\\n" | to_native').stdout, 'native:a\nnative:b\n')
            fake.write_text('#!/bin/sh\nexit 7\n')
            self.assertEqual(run('to_native /c/a').returncode, 7)


    @unittest.skipUnless(os.name == 'nt', 'cygpath belongs to Git Bash')
    def test_real_cygpath_with_msys_conversion_disabled(self):
        shell = device.git_bash()
        code = '. "' + (LIB / 'native_path.sh').as_posix() + '"; to_native "/c/space here/a.c"; printf "/c/one\\n/c/two\\n" | to_native'
        for disabled in ('0', '1'):
            result = subprocess.run([shell, '-c', code], capture_output=True, text=True,
                env=dict(os.environ, MSYS_NO_PATHCONV=disabled), timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, 'C:/space here/a.c\nC:/one\nC:/two\n')


class CaptureTests(unittest.TestCase):
    def test_result_and_sentinel_shapes(self):
        text = ':1:one:PASS\r\nf.c:2:two:FAIL: reason\n:3:skip:IGNORE\nnoise:PASS\nf.c:4:bad:PASSjunk\n'
        rows = capture.results(text)
        self.assertEqual([r['status'] for r in rows], ['PASS', 'FAIL'])
        self.assertEqual(rows[1]['message'], 'reason')
        self.assertEqual(len(capture.results(text, ignored=True)), 3)
        self.assertEqual(capture.BUILD_ID_BYTES_RE.search(b'I BUILD_ID=abc\r\n').group(1), b'abc')
        for line in (b':1:one:PASS\r\n', b'f.c:2:two:FAIL: reason\n'):
            self.assertIsNotNone(device.SUITE_RESULT.search(line))
        self.assertIsNone(device.SUITE_RESULT.search(b':1:skip:IGNORE\n'))
        for line, groups in [('I SELFTEST_COMPLETE failures=2 elapsed_ms=30', ('2', '30')),
                             ('SELFTEST_COMPLETE', (None, None))]:
            self.assertEqual(capture.SELFTEST_COMPLETE_RE.search(line).groups(), groups)
