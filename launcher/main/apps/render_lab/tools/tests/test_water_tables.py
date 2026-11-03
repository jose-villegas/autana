"""The water stage tables read lines written by the suite's own formats."""
import pathlib
import sys
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import water_tables
from build.c_header import read_define

HEADER = water_tables.LOG_HEADER
PHASES = ("rest", "shake")


def line(name, *values):
    """A log line as the device prints it: printf's ll length is Python's plain %d."""
    return "I (1) water_perf: water_perf: " + (read_define(HEADER, name).replace("%ll", "%").replace("%l", "%") % values)


def capture(configs, update_us, scale=1):
    lines = ["BUILD_ID=0123456789ab-diag"]
    for config, remeshed in configs:
        for phase in (water_tables.BEFORE, water_tables.RUNNING, water_tables.AFTER):
            lines.append(line("WATER_PERF_HEAP", config, phase, 300 * 1024, 100 * 1024))
        for phase in PHASES:
            lines.append(line("WATER_PERF_ROW", config, phase, 45, 180, remeshed, 2300, 1100, 12.5))
            lines.append(line("WATER_PERF_TIMES", config, phase, 200000 * scale, 210000, 220000, update_us * scale, 190000, 195000, 9000, 9500, 9900))
            lines.append(line("WATER_PERF_STAGE", config, phase, "pressure", 60000 * scale))
            lines.append(line("WATER_PERF_COST", config, phase,
                              "water.update 190.00/195.0  water.nets 12.00/13.0  r3d.draw 6.20/7.1  ui.paint 0.40/0.5 "
                              "| total 208.60"))
    return lines


class WaterTables(unittest.TestCase):
    def test_tables_from_suite_lines(self):
        text = "\n".join(capture((("base", 0), ("rest", 40)), 180000))
        body = water_tables.tables(text)
        self.assertIn("| rest | 45 | 4.0 | 2300 | 1100 | 200.0 / 210.0 | 180.0 / 190.0 | 9.0 / 9.5 |", body)
        self.assertIn("| step: pressure | 60.00 | 60.00 |", body)
        self.assertIn("| water.nets | 12.00 | 12.00 |", body)
        self.assertNotIn("| water.update |", body)
        self.assertIn("| r3d.draw | 6.20 | 6.20 |", body)
        self.assertNotIn("ui.paint", body)
        self.assertIn("| rest | 180.0 (40) | 180.0 (40) |", body)
        self.assertIn("| base | 300 / 100 | 300 / 100 | 300 / 100 |", body)

    def test_runs_take_mean_and_worst(self):
        first = capture((("base", 0),), 100000)
        second = capture((("base", 0),), 100000, scale=3)
        text = "\n".join(["RUN=1", *first[1:], "RUN=2", *second[1:]])
        body = water_tables.tables(text)
        self.assertIn("(2 runs", body)
        self.assertIn("| step: pressure | 120.00 | 120.00 |", body)

    def test_a_row_twice_without_run_lines_fails(self):
        once = capture((("base", 0),), 100000)
        with self.assertRaises(ValueError):
            water_tables.tables("\n".join([*once, *once[1:]]))

    def test_capture_without_base_fails(self):
        with self.assertRaises(ValueError):
            water_tables.tables("\n".join(capture((("rest", 4),), 1000)))


if __name__ == "__main__":
    unittest.main()
