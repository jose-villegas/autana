"""phase_report.py: the phase tables the device perf reporters write.

    python -m unittest discover -s launcher/tools/perf/tests
"""
import pathlib
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from phase_report import comparison_lines, phase_stats, phase_table_lines, write_phase_report  # noqa: E402

FULL = {"min": 1, "max": 9, "avg": 5000, "med": 4000, "p95": 8000}


class PhaseReportTest(unittest.TestCase):
    def test_a_phase_line_keeps_only_its_numbers(self):
        match = re.search(r"(?P<phase>Floor): avg=(?P<avg>\d+)us", "Floor: avg=6852us")
        self.assertEqual(phase_stats(match), {"avg": 6852})

    def test_a_missing_phase_or_total_reads_as_unknown_not_zero(self):
        runs = {"a": {"phases": {"Total": FULL}}, "b": {"phases": {"Total": {"avg": 0}}}}
        lines = comparison_lines(runs, ["a", "b"], ["Total", "Image"])
        self.assertIn("| Image (us) | ? | ? |", lines)
        self.assertIn("| **Total fps (avg)** | 200.0 | ? |", lines)
        self.assertIn("| **Total fps (median)** | 250.0 | ? |", lines)

    def test_an_average_only_phase_shows_dashes_for_what_was_not_logged(self):
        lines = phase_table_lines({"Total": FULL, "Floor": {"avg": 7}}, ["Total", "Floor", "Curve"])
        self.assertIn("| **Total** | 1 | 9 | 5000 | 4000 | 8000 |", lines)
        self.assertIn("| Floor | - | - | 7 | - | - |", lines)
        self.assertIn("| Curve | ? | ? | ? | ? | ? |", lines)

    def test_every_run_gets_its_own_heading_averages_and_table(self):
        runs = {"a": {"phases": {"Total": FULL}}, "b": {"phases": {"Total": dict(FULL, avg=2500)}}}
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "report.md"
            write_phase_report(out, ["# Title", ""], runs, ["a", "b"], ["Total"],
                               lambda label, run: f"## {label} run")
            lines = out.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[:2], ["# Title", ""])
        self.assertIn("| Total (us) | 5000 | 2500 |", lines)
        self.assertIn("| **Total fps (avg)** | 200.0 | 400.0 |", lines)
        for heading in ("## a run", "## b run"):
            self.assertEqual(lines[lines.index(heading) + 4].split(" | ")[0], "| **Total**")


if __name__ == "__main__":
    unittest.main()
