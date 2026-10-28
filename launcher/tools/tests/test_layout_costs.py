"""The layout table reads its rows and layouts from the captures."""
import pathlib
import sys
import tempfile
import unittest
TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "render"))
import layout_costs

MESH = """BUILD_ID=abc-dev
I (1) layout_perf: room:LAYOUT_A:frame mean 40000us p95 41000us quarter=portrait
I (2) layout_perf: room:LAYOUT_A:draw mean 30000us p95 31000us quarter=portrait
I (3) layout_perf: room:LAYOUT_B:frame mean 44000us p95 45000us quarter=portrait
I (4) layout_perf: room:LAYOUT_A:frame mean 20000us p95 21000us quarter=landscape
"""


def boot(build="abc-dev"):
    text = f"BUILD_ID={build}\n"
    for mode, total in (("LAYOUT_A", 30000), ("LAYOUT_B", 27000), ("LAYOUT_C", 33000)):
        text += (f"boot_anim_perf: === BOOT_ANIM PERF swell (now_ms=1, 2 samples, mode={mode}) ===\n"
                 f"boot_anim_perf: Total:   min={total}us max={total}us avg={total}us med={total}us p95={total}us\n")
    return text


class LayoutCostsTests(unittest.TestCase):
    def write(self, name, text):
        path = pathlib.Path(self.directory.name) / name
        path.write_text(text, encoding="utf-8")
        return path

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def test_rows_and_layouts_follow_capture_order(self):
        mesh = self.write("mesh.log", MESH)
        rows = {**layout_costs.mesh_costs(mesh), **layout_costs.boot_costs(self.write("boot.log", boot()))}
        self.assertEqual(list(rows), ["room, portrait", "room, landscape", "boot animation, swell"])
        lines = layout_costs.table(rows, "chart.png").splitlines()
        self.assertEqual(lines[0], "![Frame cost by framebuffer layout](../images/render/chart.png)")
        self.assertEqual(lines[4], "| room | 1 | `LAYOUT_B` | `LAYOUT_A` | +4.0 | +10% |")
        self.assertEqual(lines[5], "| boot animation | 1 | `LAYOUT_B` | `LAYOUT_A` | -3.0 | -10% |")
        self.assertEqual(lines[6], "| boot animation | 1 | `LAYOUT_C` | `LAYOUT_A` | +3.0 | +10% |")
        every = lines[lines.index("<details><summary>Every picture</summary>") + 2:]
        self.assertEqual(every[0], "| Picture | `LAYOUT_A` | `LAYOUT_B` | `LAYOUT_C` |")
        self.assertEqual(every[2], "| room, portrait | 40.0 | 44.0 (+10%) | not run |")
        self.assertEqual(every[3], "| room, landscape | 20.0 | not run | not run |")
        self.assertEqual(every[4], "| boot animation, swell | 30.0 | 27.0 (-10%) | 33.0 (+10%) |")
        self.assertEqual(every[6], "</details>")
        self.assertTrue(lines[-1].startswith("Board ms per frame"))

    def test_a_family_groups_a_scenes_variants_and_orientations(self):
        self.assertEqual(layout_costs.family("hall-lit-full, portrait"), "hall")
        self.assertEqual(layout_costs.family("boot animation, swell"), "boot animation")
        rows = {"hall, portrait": {"A": 10.0, "B": 12.0}, "yard, portrait": {"A": 20.0, "B": 21.0},
                "hall-lit, landscape": {"A": 30.0, "B": 35.0}}
        self.assertEqual(layout_costs.groups(rows), [("hall", "A", "B", ["hall, portrait", "hall-lit, landscape"]),
                                                     ("yard", "A", "B", ["yard, portrait"])])
        self.assertIn("| hall | 2 | `B` | `A` | +2.0 to +5.0 | +17% to +20% |", layout_costs.table(rows, "c.png"))

    def test_captures_must_share_one_build(self):
        mesh = self.write("mesh.log", MESH)
        self.assertEqual(layout_costs.require_same_build([mesh, self.write("boot.log", boot())]), "abc-dev")
        with self.assertRaisesRegex(ValueError, "different builds"):
            layout_costs.require_same_build([mesh, self.write("other.log", boot("def-dev"))])
        with self.assertRaisesRegex(ValueError, "build id"):
            layout_costs.require_same_build([self.write("bare.log", MESH.split("\n", 1)[1])])

    def test_a_rerun_keeps_the_first_report_and_missing_rows_are_refused(self):
        rerun = MESH + MESH.split("\n")[1].replace("40000us", "99000us") + "\n"
        self.assertEqual(layout_costs.mesh_costs(self.write("rerun.log", rerun))["room, portrait"]["LAYOUT_A"], 40.0)
        with self.assertRaisesRegex(ValueError, "no scene layout rows"):
            layout_costs.mesh_costs(self.write("empty.log", "BUILD_ID=abc-dev\n"))
        with self.assertRaisesRegex(ValueError, "reported twice"):
            layout_costs.boot_costs(self.write("boot.log", boot() + boot()))
        with self.assertRaisesRegex(ValueError, "no boot animation checkpoints"):
            layout_costs.boot_costs(self.write("none.log", "BUILD_ID=abc-dev\n"))


if __name__ == "__main__":
    unittest.main()
