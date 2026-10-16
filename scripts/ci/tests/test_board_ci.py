"""Board CI summaries count captured Unity results without inferring suites."""
import os
from pathlib import Path
import sys
import tempfile
import textwrap
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / 'scripts/device'), str(ROOT / 'scripts/lib')]
import device_report


class BoardCiSummaryTests(unittest.TestCase):
    def test_capture_summary(self):
        workflow = (ROOT / '.github/workflows/board-ci.yml').read_text(encoding='utf-8')
        step = workflow.split('- name: Summarize the device capture', 1)[1]
        code = textwrap.dedent(step.split("<<'PY'\n", 1)[1].split('\n          PY', 1)[0])
        capture = (Path(__file__).parent / 'fixtures/selftest.txt').read_text(encoding='utf-8')
        entry = {'command': 'selftest', 'capture_path': 'fixture.log',
                 'board': 'fixture', 'build_id': 'fixture-build'}
        passing = '\n'.join(line for line in capture.splitlines() if ':PASS' in line)
        passing += '\nSELFTEST_COMPLETE failures=0 elapsed_ms=100\n'
        for text, entries, expected in [
                (capture, [entry], 'Tests: 2 passed, 2 failed.'),
                (capture.split('SELFTEST_COMPLETE', 1)[0], [entry], 'Tests: 2 passed, 2 failed.'),
                (passing, [entry], 'Tests: 2 passed, 0 failed.'),
                ('', [], 'no self-test capture available')]:
            with self.subTest(expected=expected, complete='SELFTEST_COMPLETE' in text), \
                    tempfile.TemporaryDirectory() as directory:
                summary = Path(directory) / 'summary.md'
                with patch.dict(os.environ, GITHUB_STEP_SUMMARY=str(summary)), \
                        patch.object(device_report, 'read_index', return_value=entries), \
                        patch.object(device_report, 'read_capture_text', return_value=text), \
                        patch('device_lock.board_label', return_value='fixture board'):
                    exec(compile(code, '<board-ci-summary>', 'exec'), {})
                output = summary.read_text(encoding='utf-8')
                self.assertIn(expected, output)
                self.assertNotIn('Suites by Unity source file', output)
                if entries:
                    self.assertIn('Board: fixture board', output)
                    self.assertIn('Build ID: `fixture-build`', output)
                    self.assertEqual('Partial capture:' in output, 'SELFTEST_COMPLETE' not in text)
                    if ':FAIL:' in text:
                        self.assertIn('- FAIL `test_budget` (./main/selftest/selftest.c:85): Expected 10 Was 12', output)
                        self.assertIn('- FAIL `test_pixels` (./main/selftest/selftest.c:85): Expected TRUE Was FALSE', output)
                    else:
                        self.assertNotIn('- FAIL', output)
