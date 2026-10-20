"""The layout table reads its rows and layouts from the captures."""
import pathlib
import sys
import tempfile
import unittest
TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "render"))
import layout_costs

MESH = """BUILD_ID=abc-dev
I (1) layout_perf: room:FULL_FB:frame mean 40000us p95 41000us quarter=portrait
I (2) layout_perf: room:FULL_FB:draw mean 30000us p95 31000us quarter=portrait
I (3) layout_perf: room:DOUBLE_FB:frame mean 44000us p95 45000us quarter=portrait
I (4) layout_perf: room:FULL_FB:frame mean 20000us p95 21000us quarter=landscape
"""


def boot(build="abc-dev"):
    text = f"BUILD_ID={build}\n"
    for mode, total in (("FULL_FB", 30000), ("DOUBLE_FB", 27000), ("DOUBLE_FB_CONTENDED", 33000)):
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
        lines = layout_costs.table(rows, "abc-dev").splitlines()
        self.assertEqual(lines[0], "| Picture | `FULL_FB` | `DOUBLE_FB` | `DOUBLE_FB_CONTENDED` |")
        self.assertEqual(lines[2], "| room, portrait | 40.0 | 44.0 (+10%) | not run |")
        self.assertEqual(lines[3], "| room, landscape | 20.0 | not run | not run |")
        self.assertEqual(lines[4], "| boot animation, swell | 30.0 | 27.0 (-10%) | 33.0 (+10%) |")
        self.assertIn("build `abc-dev`", lines[-1])

    def test_captures_must_share_one_build(self):
        mesh = self.write("mesh.log", MESH)
        self.assertEqual(layout_costs.capture_build([mesh, self.write("boot.log", boot())]), "abc-dev")
        with self.assertRaisesRegex(ValueError, "different builds"):
            layout_costs.capture_build([mesh, self.write("other.log", boot("def-dev"))])
        with self.assertRaisesRegex(ValueError, "build id"):
            layout_costs.capture_build([self.write("bare.log", MESH.split("\n", 1)[1])])

    def test_repeated_or_missing_rows_are_refused(self):
        with self.assertRaisesRegex(ValueError, "twice"):
            layout_costs.mesh_costs(self.write("twice.log", MESH + MESH.split("\n")[1] + "\n"))
        with self.assertRaisesRegex(ValueError, "no scene layout rows"):
            layout_costs.mesh_costs(self.write("empty.log", "BUILD_ID=abc-dev\n"))
        with self.assertRaisesRegex(ValueError, "reported twice"):
            layout_costs.boot_costs(self.write("boot.log", boot() + boot()))
        with self.assertRaisesRegex(ValueError, "no boot animation checkpoints"):
            layout_costs.boot_costs(self.write("none.log", "BUILD_ID=abc-dev\n"))


if __name__ == "__main__":
    unittest.main()
