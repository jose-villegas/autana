"""Stress goal rounding, capture aggregation and completeness checks."""
import importlib.util
import os
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
if os.environ.get("STRESS_GOALS_MODULE"):
    spec = importlib.util.spec_from_file_location("stress_goals", os.environ["STRESS_GOALS_MODULE"])
    goals = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(goals)
else:
    import stress_goals as goals
from build.c_header import read_define

REPLAY = read_define(goals.ROOT / "launcher/test/frame_profile.h", "FRAME_PROFILE_REPLAY")


def capture(path, values, *, omit=None):
    lines = []
    for row in goals.table_rows():
        if row == omit:
            continue
        key, quarter, size = row
        lines.append(f"SUITE_TEST name=test_stress_stages/{key}/{quarter}/{size} selected=1")
        for index, stages in enumerate(values):
            report = "  ".join(f"{stage} {value}/0.00" for stage, value in zip(goals.STAGES, stages))
            lines.append(REPLAY % (key, index, "vertices_transformed=1", report + " | total 0.00")
                         + f" quarter={quarter}")
    path.write_text("\n".join(lines), encoding="utf-8")
    return path


class StressGoalsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = pathlib.Path(self.temp.name)

    def make(self, name, values, **kwargs):
        return capture(self.directory / name, values, **kwargs)

    def test_mean_then_rounds_up_in_microseconds_including_zero(self):
        path = self.make("rounding", [("0.00", "1.00", "0.10", "0.20"),
                                      ("0.02", "1.00", "0.10", "0.20")])
        line = goals.generate([path]).splitlines()[0]
        self.assertEqual('    {"stress-open", "portrait", "default", {20, 1010, 110, 210}, {1330, 1530}},', line)

    def test_worst_capture_per_stage_and_worst_sum_are_independent(self):
        a = self.make("A308", [("1.00", "0.00", "0.00", "0.00")])
        b = self.make("20E8", [("0.00", "2.00", "0.00", "0.00")])
        self.assertIn('{1010, 2020, 10, 10}, {2020, 2330}', goals.generate([a, b]).splitlines()[0])

    def test_uniform_full_keeps_zeros_for_every_quarter_and_size(self):
        path = self.make("2124", [("1.00",) * len(goals.STAGES)])
        lines = goals.generate([path]).splitlines()
        expected = goals.table_rows()
        self.assertEqual(len(expected), len(lines))
        for row, line in zip(expected, lines):
            self.assertTrue(line.startswith('    {"%s", "%s", "%s",' % tuple(row)))
            if row[0] == "stress-uniform-full":
                self.assertTrue(line.endswith('{0, 0, 0, 0}, {0, 0}},'), line)

    def test_missing_row_in_one_capture_names_capture_and_row(self):
        row = goals.table_rows()[-1]
        a = self.make("complete", [("1.00",) * len(goals.STAGES)])
        b = self.make("incomplete", [("1.00",) * len(goals.STAGES)], omit=row)
        with self.assertRaisesRegex(ValueError, "incomplete: missing row " + " ".join(row)):
            goals.generate([a, b])

    def test_capture_without_replays_is_refused_by_name(self):
        path = self.directory / "empty"
        path.write_text("booted\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "empty: no FRAME_PROFILE_REPLAY lines"):
            goals.generate([path])

    def test_missing_stage_is_refused_by_name(self):
        path = self.make("partial", [("1.00",)])
        with self.assertRaisesRegex(ValueError, "partial: stress-open portrait default: missing stages.*r3d.draw"):
            goals.generate([path])

    def test_fixed_sizes_have_their_own_measurements(self):
        a = self.make("sizes", [("1.00",) * len(goals.STAGES)])
        text = a.read_text(encoding="utf-8")
        start = text.index("SUITE_TEST name=test_stress_stages/stress-uniform-mid/portrait/368x448")
        end = text.index("SUITE_TEST", start + len("SUITE_TEST"))
        a.write_text(text[:start] + text[start:end].replace("1.00/0.00", "2.00/0.00") + text[end:], encoding="utf-8")
        result = goals.generate([a])
        self.assertIn('"stress-uniform-mid", "portrait", "368x448", {2020, 2020, 2020, 2020}', result)
        self.assertIn('"stress-uniform-mid", "portrait", "default", {1010, 1010, 1010, 1010}', result)


if __name__ == "__main__":
    unittest.main()
