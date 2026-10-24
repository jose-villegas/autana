"""Board report parsing keeps build ownership and omits tests without a COST."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "r3d/skin_light"))
from report_skin_light_board import rows, lighting_modes


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
        self.assertEqual(result[1][-2:], ["—", "—"])

    def test_flush_at_next_test_build_and_end_and_build_cost(self):
        text = """BUILD_ID first-diag
skin_light_perf: POSE vertices=4
skin_light_perf: test_build med=40cycles
COST cycles_per_build=40
skin_light_perf: test_vertex med=12cycles
COST cycles_per_vertex=12
BUILD_ID second-diag
skin_light_perf: test_last med=8cycles
COST cycles_per_vertex=8
"""
        result = list(rows(text))
        self.assertEqual([r[:5] for r in result], [
            ["first-diag", "build", "40.000", "build", "10.000"],
            ["first-diag", "vertex", "12.000", "vertex", "—"],
            ["second-diag", "last", "8.000", "vertex", "—"],
        ])

    def test_repeated_runs_use_median_per_build_and_kernel(self):
        text = "BUILD_ID first-diag\n"
        for cost in (50, 10, 30, 20, 40):
            text += (f"POSE vertices=2\nskin_light_perf: test_build med={cost}cycles\n"
                     f"COST cycles_per_build={cost}\nQUALITY max={cost} mean={cost / 100}\n")
        text += "BUILD_ID second-diag\nskin_light_perf: test_build med=8cycles\nCOST cycles_per_build=8\n"
        result = list(rows(text))
        self.assertEqual(len(result), 2)
        self.assertEqual(result[0][:5], ["first-diag", "build", "30.000", "build", "15.000"])
        self.assertEqual(result[0][-2:], ["50", "0.5"])
        self.assertEqual(result[1][:3], ["second-diag", "build", "8.000"])

    def test_mode_table_uses_runtime_build_and_ram(self):
        text = """BUILD_ID runtime-diag
POSE vertices=4
RAM mode=table renderer_bytes=80 scene_bytes=1920 scene_metadata_psram=124
skin_light_perf: test_normal_palette_build med=40cycles
COST cycles_per_build=40
skin_light_perf: test_fused_table_1 med=400cycles
COST cycles_per_vertex=100
QUALITY max=8 mean=0.5
skin_light_perf: test_table_build_1 med=200cycles
COST cycles_per_build=200
"""
        table = lighting_modes(text)
        self.assertIn("| table | 1 | 8 | 200 | 1920 |", table)
        self.assertIn("In every row the normal palette costs 40 cycles", table)
        self.assertNotIn("Normal-palette", table)

    def test_modes_require_table_capture(self):
        with self.assertRaisesRegex(ValueError, "table"):
            lighting_modes("BUILD_ID empty-diag\n")

    def test_modes_use_latest_table_build_memory_and_derived_counts(self):
        def capture(build, memory, costs):
            text = f"BUILD_ID {build}\nPOSE vertices=4\nRAM mode=table renderer_bytes={memory} scene_bytes=20 scene_metadata_psram=30\n"
            for cost in costs:
                text += f"skin_light_perf: test_fused_table_3 med=1cycles\nCOST cycles_per_vertex={cost}\nQUALITY max=1 mean=0.1\n"
            text += "skin_light_perf: test_table_build_3 med=1cycles\nCOST cycles_per_build=1\n"
            return text
        text = capture("old-diag", 10, [100]) + capture("new-diag", 40, [2, 4])
        table = lighting_modes(text)
        self.assertIn("| table | 3 | 1 | 1 | 20 |", table)
        self.assertIn("the renderer holds 40 bytes", table)
        self.assertNotIn("10 bytes", table)

    def test_modes_require_table_build_cost(self):
        with self.assertRaisesRegex(ValueError, "table"):
            lighting_modes("BUILD_ID empty-diag\nskin_light_perf: test_fused_table_1 med=1cycles\nCOST cycles_per_vertex=1\n")

    def test_a_column_that_differs_between_modes_stays_in_the_table(self):
        text = """BUILD_ID a-diag
POSE vertices=4
RAM mode=direct renderer_bytes=80 scene_bytes=10 scene_metadata_psram=30
RAM mode=table renderer_bytes=90 scene_bytes=20 scene_metadata_psram=30
skin_light_perf: test_fused_direct_1 med=1cycles
COST cycles_per_vertex=1
QUALITY max=1 mean=0.1
skin_light_perf: test_fused_table_1 med=1cycles
COST cycles_per_vertex=1
QUALITY max=1 mean=0.1
skin_light_perf: test_table_build_1 med=1cycles
COST cycles_per_build=1
"""
        table = lighting_modes(text)
        self.assertIn("Renderer internal bytes", table)
        self.assertIn("| direct | 1 | 1 | — | 10 | 80 |", table)
        self.assertIn("the scene holds 30 bytes of PSRAM", table)
        self.assertNotIn("renderer holds", table)

    def test_comment_lines_are_ignored(self):
        self.assertEqual(list(rows("# Capture comments\n")), [])


if __name__ == "__main__":
    unittest.main()
