"""Threshold-only report comparison and summary contracts."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]


class CompareReportsTest(unittest.TestCase):
    def verdict(self, old, new, threshold):
        with tempfile.TemporaryDirectory() as root:
            paths = []
            for label, rows in [('old', old), ('new', new)]:
                path = pathlib.Path(root)/(label+'.md')
                path.write_text('| Test | Measured (us) |\n|---|---:|\n' +
                                ''.join(f'| `{name}` | {value} |\n' for name, value in rows.items()))
                paths.append(str(path))
            return subprocess.run([sys.executable, str(TOOLS/'compare_reports.py'), *paths,
                                   '--verdict', '--threshold', str(threshold)], capture_output=True, text=True)

    def test_verdict_uses_threshold_without_control_rows(self):
        result = self.verdict({'row': 10000}, {'row': 9900}, .4)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('threshold=0.4%', result.stdout)
        self.assertNotIn('floor=', result.stdout)
        result = self.verdict({'row': 10000}, {'row': 9900}, 2)
        self.assertEqual(result.returncode, 1)

    def test_absolute_guard_and_regression_prevent_win(self):
        result = self.verdict({'tiny': 3}, {'tiny': 1}, .1)
        self.assertEqual(result.returncode, 1)
        result = self.verdict({'up': 1000, 'down': 1000}, {'up': 1200, 'down': 800}, .1)
        self.assertEqual(result.returncode, 1)
        self.assertIn('regressed=1', result.stdout)

    def test_move_exactly_at_threshold_is_not_counted(self):
        for value in (9900, 10100):
            result = self.verdict({'row': 10000}, {'row': value}, 1)
            self.assertEqual(result.returncode, 1)
            self.assertIn('improved=0 regressed=0', result.stdout)

    def test_table_includes_every_row_without_control_section(self):
        with tempfile.TemporaryDirectory() as root:
            path = pathlib.Path(root)/'table.md'
            path.write_text('| Test | Measured (us) |\n|---|---:|\n| `row` | 100 |\n')
            result = subprocess.run([sys.executable, str(TOOLS/'compare_reports.py'), str(path), str(path)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('of 1 rows', result.stdout)
            self.assertNotIn('Controls', result.stdout)
            self.assertNotIn('layout?', result.stdout)

    def test_summary_has_no_row_allowlist(self):
        script = (TOOLS/'report_performance.sh').read_text()
        function = script[script.index('report_summary()'):script.index('# shellcheck source=')]
        result = subprocess.run(['sh', '-c', 'BASELINE=""\n' + function + '\nreport_summary capture table'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '=== Summary ===')


if __name__ == '__main__':
    unittest.main()
