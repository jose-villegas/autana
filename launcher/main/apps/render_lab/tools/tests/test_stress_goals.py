"""The stress goal table: generated from the suite's own PERF TARGET lines, and matching the suite's cases."""
import importlib.util
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
SCRATCH_MODULE = os.environ.get("STRESS_GOALS_MODULE")
if SCRATCH_MODULE:
    spec = importlib.util.spec_from_file_location("stress_goals", SCRATCH_MODULE)
    goals = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(goals)
else:
    import stress_goals as goals
from build.c_header import read_define

SCRIPT = pathlib.Path(SCRATCH_MODULE) if SCRATCH_MODULE else HERE.parent / "stress_goals.py"
PRESETS = HERE.parents[1] / "stress" / "stress_preset.h"
DISPLAY = goals.ROOT / "launcher/main/display/display.h"
UNPEGGED = "stress-uniform-full"
# perf_goal's two line forms (launcher/test/perf_budget.h), behind an ESP-IDF log prefix.
UNPEGGED_LINE = "I (1) device_tests: PERF TARGET %s: measured %d us, goal not yet pegged"
PEGGED_LINE = "I (1) device_tests: PERF TARGET %s: measured %d us, goal 1 us, distance +0%%"
STAGE_US = 1000
SUM_US = 3000


def names(row):
    sum_name = goals.row_name(row)
    return [sum_name] + [f"{sum_name} {stage}" for stage in goals.STAGES]


def capture_text(sums=None, stages=None, omit=(), pegged=False):
    """Every row's targets: the sum at `sums[row]` and each stage at `stages[(row, stage)]`."""
    form = PEGGED_LINE if pegged else UNPEGGED_LINE
    lines = []
    for row in goals.table_rows():
        sum_name, *stage_names = names(row)
        if sum_name not in omit:
            lines.append(form % (sum_name, (sums or {}).get(row, SUM_US)))
        for stage, name in zip(goals.STAGES, stage_names):
            if name not in omit:
                lines.append(form % (name, (stages or {}).get((row, stage), STAGE_US)))
    return "\n".join(lines) + "\n"


def line_of(output, row):
    return next(line for line in output.splitlines() if line.startswith('    {"%s", "%s", "%s",' % row))


class StressGoalsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = pathlib.Path(self.temp.name)
        self.first = goals.table_rows()[0]

    def write(self, name, text):
        path = self.directory / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_the_stages_are_the_suite_s_list_r3d_s_then_the_recorder_s(self):
        self.assertEqual(goals.STAGES, ("r3d.cull", "r3d.transform", "r3d.draw", "r3d.upscale", "frame.record"))

    def test_margin_rounds_up_to_the_quantum_and_a_zero_still_pegs(self):
        stages = {(self.first, goals.STAGES[0]): 0, (self.first, goals.STAGES[1]): 1000,
                  (self.first, goals.STAGES[2]): 991}
        path = self.write("rounding", capture_text(sums={self.first: 2000}, stages=stages))
        # 0 -> the quantum; 1000 x 1.01 = 1010 exactly; 991 x 1.01 = 1000.91 -> 1010.
        self.assertIn("{10, 1010, 1010, 1010, 1010}, {2020, 2330}},", line_of(goals.generate([path]), self.first))

    def test_worst_per_stage_and_worst_sum_come_from_different_captures(self):
        a = self.write("a", capture_text(sums={self.first: 5000}, stages={(self.first, goals.STAGES[0]): 2000}))
        b = self.write("b", capture_text(sums={self.first: 2000}, stages={(self.first, goals.STAGES[1]): 3000}))
        line = line_of(goals.generate([a, b]), self.first)
        self.assertIn("{2020, 3030, 1010, 1010, 1010}, {5050, 5810}},", line)

    def test_a_rerun_case_keeps_its_worst_measure(self):
        sum_name = goals.row_name(self.first)
        text = capture_text() + UNPEGGED_LINE % (sum_name, 4000) + "\n" + UNPEGGED_LINE % (sum_name, 1000) + "\n"
        self.assertIn("{4040, 4650}},", line_of(goals.generate([self.write("rerun", text)]), self.first))

    def test_pegged_and_unpegged_lines_read_alike(self):
        a = goals.generate([self.write("unpegged", capture_text())])
        b = goals.generate([self.write("pegged", capture_text(pegged=True))])
        self.assertEqual(a, b)

    def test_unpegged_preset_prints_zeros_and_others_do_not(self):
        output = goals.generate([self.write("c", capture_text())], [UNPEGGED])
        rows = goals.table_rows()
        self.assertEqual(len(rows), len(output.splitlines()))
        zeros = "{%s}, {0, 0}}," % ", ".join(["0"] * len(goals.STAGES))
        for row, line in zip(rows, output.splitlines()):
            self.assertTrue(line.startswith('    {"%s", "%s", "%s",' % row), line)
            self.assertEqual(row[0] == UNPEGGED, line.endswith(zeros), line)

    def test_unknown_unpegged_preset_is_refused(self):
        with self.assertRaisesRegex(ValueError, "--unpegged stress-nope: no such preset"):
            goals.generate([self.write("c", capture_text())], ["stress-nope"])

    def test_missing_target_names_the_capture_and_the_target(self):
        missing = names(goals.table_rows()[-1])[2]
        a = self.write("complete", capture_text())
        b = self.write("incomplete", capture_text(omit={missing}))
        with self.assertRaisesRegex(ValueError, "incomplete: missing " + re.escape(missing)):
            goals.generate([a, b])

    def test_capture_with_no_targets_is_refused(self):
        with self.assertRaisesRegex(ValueError, "empty: missing "):
            goals.generate([self.write("empty", "booted\n")])

    def test_a_stress_target_with_no_goal_row_is_refused(self):
        text = capture_text() + UNPEGGED_LINE % ("stress-gone portrait", 1) + "\n"
        with self.assertRaisesRegex(ValueError, "stale: target stress-gone portrait has no goal row"):
            goals.generate([self.write("stale", text)])

    def test_other_suites_targets_change_nothing(self):
        plain = goals.generate([self.write("plain", capture_text())])
        foreign = capture_text() + UNPEGGED_LINE % ("sponza portrait r3d.draw", 99999) + "\n"
        self.assertEqual(plain, goals.generate([self.write("foreign", foreign)]))

    def test_row_names_are_the_ones_report_goals_prints(self):
        """report_goals() names a row "<key> <quarter>", adding the size only for a ladder step."""
        sized = next(row for row in goals.table_rows() if row[2] != goals.DEFAULT_SIZE)
        self.assertEqual(f"{self.first[0]} {self.first[1]}", goals.row_name(self.first))
        self.assertEqual(" ".join(sized), goals.row_name(sized))

    def test_sizes_are_their_own_rows(self):
        sized = next(row for row in goals.table_rows() if row[2] != goals.DEFAULT_SIZE)
        default = (sized[0], sized[1], goals.DEFAULT_SIZE)
        output = goals.generate([self.write("sizes", capture_text(sums={sized: 9000}))])
        self.assertIn("{9090, 10460}},", line_of(output, sized))
        self.assertIn("{3030, 3490}},", line_of(output, default))

    def test_table_rows_are_the_cases_the_suite_registers(self):
        """Every preset in both quarters; the uniform presets also at each ladder size."""
        presets = re.findall(r'^\s*X\(\w+, "([^"]+)", (\w+)\)', PRESETS.read_text(encoding="utf-8"), re.MULTILINE)
        quarters = [read_define(DISPLAY, name) for name in ("DISPLAY_PORTRAIT_NAME", "DISPLAY_LANDSCAPE_NAME")]
        rows = goals.table_rows()
        ladder = sorted({size for _, _, size in rows if size != goals.DEFAULT_SIZE})
        expected = [(f"stress-{key}", quarter, size) for key, surface in presets for quarter in quarters
                    for size in [goals.DEFAULT_SIZE] + (ladder if surface == "UNIFORM" else [])]
        self.assertTrue(ladder)
        self.assertEqual(sorted(expected), sorted(rows))

    def test_duplicate_or_short_table_rows_are_refused(self):
        real = goals.SUITE.read_text(encoding="utf-8")
        row = re.search(r'^    \{"stress-open", "portrait".*$', real, re.MULTILINE)[0]
        cases = (("duplicate", real.replace(row, row + "\n" + row, 1)),
                 ("short", real.replace(row, re.sub(r"\{(\d+), ", "{", row, count=1), 1)))
        for name, text in cases:
            with self.subTest(name):
                scratch = self.write(f"suite_{name}.c", text)
                original = goals.SUITE
                goals.SUITE = scratch
                try:
                    with self.assertRaisesRegex(ValueError, "duplicate|stage goals"):
                        goals.table_rows()
                finally:
                    goals.SUITE = original

    def test_command_prints_only_the_table_and_refusals_exit_one(self):
        path = self.write("c", capture_text())
        run = subprocess.run([sys.executable, str(SCRIPT), str(path), "--unpegged", UNPEGGED],
                             stdin=subprocess.DEVNULL, capture_output=True, text=True, check=False)
        self.assertEqual((0, ""), (run.returncode, run.stderr))
        self.assertEqual(goals.generate([path], [UNPEGGED]) + "\n", run.stdout)
        bad = subprocess.run([sys.executable, str(SCRIPT), str(self.write("empty", ""))],
                             stdin=subprocess.DEVNULL, capture_output=True, text=True, check=False)
        self.assertEqual((1, ""), (bad.returncode, bad.stdout))
        self.assertIn("empty: missing", bad.stderr)


if __name__ == "__main__":
    unittest.main()
