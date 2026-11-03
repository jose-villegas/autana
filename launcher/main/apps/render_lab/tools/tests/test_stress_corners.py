"""Each stress preset reaches its corner in a host render, by the cull view's counts.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_stress_corners.py

Builds the host renderer once and draws every preset stress_preset.h names
with --view cull --counts, then holds each to what it is the loss case of:
open hides and culls nothing, stack hides most of what it keeps, tiny copies
are all in view, big tris draws every triangle it submits. The sizes the
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


def preset_keys():
    """The preset keys, in the order STRESS_PRESETS lists them."""
    return re.findall(r'^\s*X\(context, \w+, "([^"]+)"', PRESETS.read_text(), re.MULTILINE)


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
        self.assertEqual({"open", "stack", "tiny-copies", "big-tris", "per-frame"}, set(preset_keys()))

    def test_open_hides_and_culls_nothing(self):
        c = self.counts("open")
        self.assertGreater(c["clusters_drawn"], 0)
        self.assertEqual(0, c["clusters_hidden"])
        self.assertEqual(0, c["nodes_cut"])
        self.assertEqual(c["clusters_tested"], c["clusters_kept"])

    def test_stack_hides_most_of_what_it_keeps(self):
        c = self.counts("stack")
        self.assertGreater(c["clusters_hidden"], c["clusters_drawn"])

    def test_tiny_copies_are_all_in_view(self):
        c = self.counts("tiny-copies")
        self.assertEqual(0, c["nodes_cut"])
        self.assertEqual(c["clusters_tested"], c["clusters_kept"])

    def test_big_tris_draws_every_triangle_it_submits(self):
        c = self.counts("big-tris")
        self.assertGreater(c["triangles_submitted"], 0)
        self.assertEqual(c["triangles_submitted"], c["triangles_covering"])

    def test_per_frame_draws_the_whole_wave(self):
        c = self.counts("per-frame")
        self.assertGreater(c["triangles_submitted"], 0)
        self.assertEqual(c["triangles_submitted"], c["triangles_covering"])


if __name__ == "__main__":
    unittest.main()
