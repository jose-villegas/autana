"""Board report parsing keeps build ownership and omits tests without a COST."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "r3d/skin_light"))
from report_skin_light_board import rows


class BoardRows(unittest.TestCase):
    def test_builds_and_tests_without_cost_do_not_leak_labels(self):
        text = """BUILD_ID first-diag
skin_light_perf: POSE vertices=984
skin_light_perf: test_fused_1 med=10cycles
COST cycles_per_vertex=10
QUALITY max=1 mean=0.1
skin_light_perf: test_no_cost med=20cycles
BUILD_ID second-diag
skin_light_perf: test_fused_4 med=30cycles
COST cycles_per_vertex=30
"""
        result = list(rows(text))
        self.assertEqual([(r[0], r[1], r[2]) for r in result],
                         [("first-diag", "fused_1", "10.000"), ("second-diag", "fused_4", "30.000")])
        self.assertEqual(result[0][-2:], ["1", "0.1"])

    def test_pending_current_capture_slot_is_optional(self):
        self.assertEqual(list(rows("# CURRENT SUITE CAPTURE PENDING\n")), [])


if __name__ == "__main__":
    unittest.main()
