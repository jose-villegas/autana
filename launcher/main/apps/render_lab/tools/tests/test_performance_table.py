"""The report table preserves the capture's stage identities."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PERF = Path(__file__).resolve().parents[5] / "tools/perf"
sys.path.insert(0, str(PERF))
from layout_measure import parse_report, parse_timings


class PerformanceTableTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_table_mode_preserves_all_stage_rows(self):
        path = Path(__file__).resolve().parents[1] / "report_performance.py"
        capture = self.root / "fixture.log"
        text = "\n".join(f"capybara_perf: stage=frame paused={paused} quarter={quarter} mean_us={412+paused}"
                         for quarter in ("portrait", "landscape") for paused in (0, 1))
        text += "\ncapybara_perf: stage=skin cores=2 mean_us=17\ncapybara_perf: stage=custom mean_us=19\n"
        capture.write_text(text)
        table = self.root / "table.md"
        subprocess.run([sys.executable, str(path), str(capture), str(table), "--table"], check=True, capture_output=True)
        self.assertEqual(parse_report(table), parse_timings(text))
        self.assertEqual(len(parse_report(table)), 6)



if __name__ == "__main__":
    unittest.main()
