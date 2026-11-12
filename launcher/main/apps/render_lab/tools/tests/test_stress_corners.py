"""Each stress preset reaches its corner in a host render, by the cull view's counts.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_stress_corners.py

Builds the host renderer once and draws every preset stress_preset.h names
with --view cull --counts, then holds each to what it is the loss case of:
open hides and culls nothing, stack hides most of what it keeps, tiny copies
are all in view and all drawn, big tris draws every triangle it submits,
two-sided cubes skip nothing facing away, noisy tiny cylinders are all in
view with every copy drawn, and
before a still camera only the per-frame preset's frames differ. The sizes the
triangles are drawn at, and the small-fill gate failing noisy-tiny's slivers,
are suite_stress_preset.c's to hold.
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
sys.path.insert(0, str(HERE.parents[5] / "launcher/tools"))

PRESETS = HERE.parents[1] / "stress" / "stress_preset.h"
TABLE = HERE.parents[1] / "stress" / "stress_preset.c"
FRAMES = 2
WATER_UNIFORM_PIXELS = 41700
EXISTING_UNIFORM_PIXELS = 5300
DENSE_FACTOR = 8  # a few-copies copy holds this many times a tiny-copies copy's clusters and triangles
STATIC_COPY =HERE.parents[5] / "launcher/demo/stress/slot.obj"
STATIC_VERTICES = sum(line.startswith("v ") for line in STATIC_COPY.read_text().splitlines())


def preset_copies():
    """Each preset's copy count by its key, in the order STRESS_PRESETS lists them."""
    keys = re.findall(r'^\s*X\((\w+), "([^"]+)"', PRESETS.read_text(), re.MULTILINE)
    rows = dict(re.findall(r'^\s*\[STRESS_(\w+)\] = \{\w+, \d+, (\d+),', TABLE.read_text(), re.MULTILINE))
    return {key: int(rows[name]) for name, key in keys}


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

    def counts(self, key, *options):
        """The last frame's counts of preset `key`, by name."""
        rows = self.out / (key + ".csv")
        run = subprocess.run([str(self.binary), "--quarter", "1", "--no-hud", "--frames", str(FRAMES), *options, "--scene",
                              "stress-" + key, "--view", "cull", "--counts", str(rows), "-o",
                              str(self.out / (key + ".bmp"))], capture_output=True, text=True, timeout=120)
        self.assertEqual(0, run.returncode, run.stderr[-2000:])
        with rows.open(newline="") as f:
            last = list(csv.DictReader(f))[-1]
        return {name: int(value) for name, value in last.items()}

    def test_every_preset_is_held_here(self):
        self.assertEqual({"open", "stack", "tiny-copies", "big-tris", "per-frame", "two-sided", "noisy-tiny", "mix",
                          "static-copies", "uniform-mid", "uniform-full", "card", "culled", "near",
                          "few-copies", "cull-setup"}, set(preset_copies()))

    def assert_all_in_view(self, c):
        """Nothing culled: every cluster tested is kept, and no node is cut."""
        self.assertEqual(0, c["nodes_cut"])
        self.assertEqual(c["clusters_tested"], c["clusters_kept"])

    def assert_draws_every_triangle(self, key):
        c = self.counts(key)
        self.assertGreater(c["triangles_submitted"], 0)
        self.assertEqual(c["triangles_submitted"], c["triangles_covering"])

    def test_static_copies_keep_the_pack_mesh(self):
        c = self.counts("static-copies")
        self.assertGreater(c["clusters_kept"], 0)
        self.assertEqual(0, c["triangles_facing_away"])
        self.assertEqual(c["triangles_submitted"], c["triangles_covering"])
        self.assertEqual(c["triangles_submitted"], c["clusters_kept"])
        self.assertEqual(c["vertices_transformed"], STATIC_VERTICES * c["clusters_kept"])
        self.assertGreater(self.counts("open")["triangles_submitted"], c["triangles_submitted"])

    def test_uniform_area_spans_water_without_shaded_pixels(self):
        middle = self.counts("uniform-mid", "--size", "368x224")
        full = self.counts("uniform-full", "--size", "368x224")
        for c in (middle, full):
            self.assertGreater(c["pixels_uniform"], 0)
            self.assertEqual(0, c["pixels_shaded"])
            self.assertEqual(0, c["pixels_face"])
            self.assertGreater(c["sized_uniform_more"], 0)
        self.assertGreater(full["pixels_uniform"], WATER_UNIFORM_PIXELS)
        self.assertGreater(middle["pixels_uniform"], EXISTING_UNIFORM_PIXELS)
        self.assertLess(middle["pixels_uniform"], WATER_UNIFORM_PIXELS)

    def test_upscale_counts_describe_actual_compose(self):
        half = self.counts("uniform-mid")
        wide = self.counts("uniform-mid", "--size", "368x224")
        self.assertEqual(184 * 224, half["render_width"] * half["render_height"])
        self.assertEqual(368 * 448, half["output_width"] * half["output_height"])
        self.assertEqual(368 * 224, wide["render_width"] * wide["render_height"])
        self.assertEqual(368 * 448, wide["output_width"] * wide["output_height"])
        from render.cull_counts import UPSCALE_PATHS
        self.assertEqual("kept", UPSCALE_PATHS[wide["upscale_path"]])

    def test_mix_fills_every_uniform_and_shaded_size_bin(self):
        c = self.counts("mix")
        for mode in ("uniform", "shaded"):
            for size in ("zero", "one", "two_to_four", "more"):
                self.assertGreater(c[f"sized_{mode}_{size}"], 0, f"{mode}/{size}")
        self.assertEqual(c["triangles_set_up"], sum(value for name, value in c.items() if name.startswith("sized_")))

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

    def test_cull_setup_walks_almost_nothing(self):
        """One copy of a few clusters: the cull's walk is a few nodes, so what it costs is its setup."""
        self.assertEqual(1, preset_copies()["cull-setup"])
        c = self.counts("cull-setup")
        self.assert_all_in_view(c)
        tiny = self.counts("tiny-copies")
        self.assertLessEqual(c["clusters_kept"], tiny["clusters_kept"] / preset_copies()["tiny-copies"])

    def test_few_copies_give_each_core_one_dense_copy(self):
        """Two copies, each many clusters: the cull's split puts one whole copy on each core."""
        copies = preset_copies()["few-copies"]
        self.assertEqual(2, copies)
        c = self.counts("few-copies")
        self.assert_all_in_view(c)
        tiny = self.counts("tiny-copies")
        tiny_clusters_per_copy = tiny["clusters_kept"] / preset_copies()["tiny-copies"]
        tiny_triangles_per_copy = tiny["triangles_submitted"] / preset_copies()["tiny-copies"]
        # Dense: each copy holds a multiple of what a tiny copy does, in clusters and in triangles.
        self.assertGreater(c["clusters_kept"] / copies, DENSE_FACTOR * tiny_clusters_per_copy)
        self.assertGreater(c["triangles_submitted"] / copies, DENSE_FACTOR * tiny_triangles_per_copy)
        # Equal weight: the same shape twice, so every total splits evenly between the two copies.
        for name in ("clusters_kept", "triangles_submitted", "vertices_transformed"):
            self.assertEqual(0, c[name] % copies, name)

    def test_two_sided_skips_nothing_facing_away(self):
        """A closed shape drawn from both sides: the one-sided spheres skip their far half, these cubes none."""
        self.assertGreater(self.counts("tiny-copies")["triangles_facing_away"], 0)
        c = self.counts("two-sided")
        self.assertGreater(c["triangles_submitted"], 0)
        self.assertEqual(0, c["triangles_facing_away"])
        self.assertGreater(c["clusters_hidden"], 0)

    def test_noisy_tiny_copies_are_all_in_view(self):
        c = self.counts("noisy-tiny")
        self.assertGreaterEqual(c["clusters_drawn"], preset_copies()["noisy-tiny"])
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
