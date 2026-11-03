"""Each stress preset reaches its corner in a host render, by the cull view's counts.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_stress_corners.py

Builds the host renderer once and draws every preset stress_preset.h names
with --view cull --counts, then holds each to what it is the loss case of:
open hides and culls nothing, stack hides most of what it keeps, tiny copies
are all in view and all drawn, big tris draws every triangle it submits, and
before a still camera only the per-frame preset's frames differ. The sizes the
triangles are drawn at are suite_stress_preset.c's to hold.
"""
import csv
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import host_render  # noqa: E402

PRESETS = HERE.parents[1] / "stress" / "stress_preset.h"
FRAMES = 2


def preset_copies():
    """Each preset's copy count by its key, in the order STRESS_PRESETS lists them."""
    rows = re.findall(r'^\s*X\(\w+, "([^"]+)", \w+, \w+, \d+, (\d+),', PRESETS.read_text(), re.MULTILINE)
    return {key: int(copies) for key, copies in rows}


class StressCorners(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = pathlib.Path(tempfile.mkdtemp(prefix="stress_corners_"))
        run, cls.binary = host_render.run(cls.out, "--build-only")
        cls.build_output = run.stdout + run.stderr
        cls.built = run.returncode == 0

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def setUp(self):
        self.assertTrue(self.built, self.build_output[-2000:])

    def frames(self, key):
        """Preset `key`'s last two frames as the still camera sees them, as raw raster bytes."""
        dump = self.out / ("still-" + key)
        dump.mkdir()
        run = subprocess.run([str(self.binary), "--quarter", "1", "--no-hud", "--frames", str(FRAMES + 1), "--scene",
                              "stress-" + key, "--camera", "still", "--dump", str(dump), "-o",
                              str(dump / "last.bmp")], capture_output=True, text=True, timeout=120)
        self.assertEqual(0, run.returncode, run.stderr[-2000:])
        return [(dump / ("%04d.raw" % i)).read_bytes() for i in (FRAMES - 1, FRAMES)]

    def counts(self, key):
        """The last frame's counts of preset `key`, by name."""
        rows = self.out / (key + ".csv")
        run = subprocess.run([str(self.binary), "--quarter", "1", "--no-hud", "--frames", str(FRAMES), "--scene",
                              "stress-" + key, "--view", "cull", "--counts", str(rows), "-o",
                              str(self.out / (key + ".bmp"))], capture_output=True, text=True, timeout=120)
        self.assertEqual(0, run.returncode, run.stderr[-2000:])
        with rows.open(newline="") as f:
            last = list(csv.DictReader(f))[-1]
        return {name: int(value) for name, value in last.items()}

    def test_every_preset_is_held_here(self):
        self.assertEqual({"open", "stack", "tiny-copies", "big-tris", "per-frame"}, set(preset_copies()))

    def assert_all_in_view(self, c):
        """Nothing culled: every cluster tested is kept, and no node is cut."""
        self.assertEqual(0, c["nodes_cut"])
        self.assertEqual(c["clusters_tested"], c["clusters_kept"])

    def assert_draws_every_triangle(self, key):
        c = self.counts(key)
        self.assertGreater(c["triangles_submitted"], 0)
        self.assertEqual(c["triangles_submitted"], c["triangles_covering"])

    def test_open_hides_and_culls_nothing(self):
        c = self.counts("open")
        self.assertGreater(c["clusters_drawn"], 0)
        self.assertEqual(0, c["clusters_hidden"])
        self.assert_all_in_view(c)

    def test_stack_hides_most_of_what_it_keeps(self):
        c = self.counts("stack")
        self.assertGreater(c["clusters_drawn"], 0)
        self.assertGreater(c["clusters_hidden"], c["clusters_drawn"])

    def test_tiny_copies_are_all_in_view(self):
        c = self.counts("tiny-copies")
        self.assertGreaterEqual(c["clusters_drawn"], preset_copies()["tiny-copies"])
        self.assert_all_in_view(c)

    def test_big_tris_draws_every_triangle_it_submits(self):
        self.assert_draws_every_triangle("big-tris")

    def test_per_frame_draws_the_whole_wave(self):
        self.assert_draws_every_triangle("per-frame")

    def test_only_per_frame_moves_before_a_still_camera(self):
        before, after = self.frames("per-frame")
        self.assertNotEqual(before, after)
        before, after = self.frames("stack")
        self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
