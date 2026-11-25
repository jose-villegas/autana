"""Each stress preset reaches its corner in a host render, by the cull view's counts.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_stress_corners.py

Builds the host renderer once and draws every preset stress_preset.h names
with --view cull --counts, then holds each to what it is the loss case of:
open hides and culls nothing, stack hides most of what it keeps, tiny copies
are all in view and all drawn, big tris draws every triangle it submits,
two-sided cubes skip nothing facing away, noisy tiny cylinders are all in
view with every copy drawn, and hinged butterflies keep every part in view
and submit every triangle. Before a still camera, wave and hinged presets
change frames. Triangle sizes and noisy-tiny's sliver gate are held by
suite_stress_preset.c. The baked ring keeps and cuts clusters throughout
its camera turn; cull-setup draws one small cube.
"""
import csv
import collections
import math
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
from test_render_views import read_bmp  # noqa: E402
sys.path.insert(0, str(HERE.parents[5] / "launcher/tools"))
sys.path.insert(0, str(HERE.parents[5] / "launcher/tools/device"))
import gfx_color  # noqa: E402

from build.c_header import read_define  # noqa: E402
from build.find_cc import host_cc  # noqa: E402
from geometry.vectors import qrot  # noqa: E402
from r3d.framegen.warp import R3D_DEPTH_EMPTY, read_dump  # noqa: E402

PRESETS = HERE.parents[1] / "stress" / "stress_preset.h"
TABLE = HERE.parents[1] / "stress" / "stress_preset.c"
FRAMES = 2
SWEEP_FRAMES = 8
CONTACT_VIEW_DT_MS = 100
CONTACT_VIEW_STEERING = ("0:fly 0 1 0 0 80 20", "1:fly 0 0 0 0 0 0")
WATER_UNIFORM_PIXELS = 41700
EXISTING_UNIFORM_PIXELS = 5300
DENSE_FACTOR = 8  # a few-copies copy holds this many times a tiny-copies copy's clusters and triangles
STATIC_COPY = HERE.parents[5] / "launcher/demo/stress/slot.obj"
BUTTERFLY_DIR = HERE.parents[5] / "launcher/demo/butterfly"
BUTTERFLY_PARTS = ("butterfly_body.obj", "butterfly_wing.obj", "butterfly_wing.obj")
BUTTERFLY_TRIANGLES = sum(sum(line.startswith("f ") for line in (BUTTERFLY_DIR / part).read_text().splitlines())
                          for part in BUTTERFLY_PARTS)
STATIC_VERTICES = sum(line.startswith("v ") for line in STATIC_COPY.read_text().splitlines())


def preset_copies():
    """Each preset's copy count by its key, in the order STRESS_PRESETS lists them."""
    constants = dict(re.findall(r"^#define\s+(\w+)\s+(\d+)$", PRESETS.read_text(), re.MULTILINE))
    keys = re.findall(r'^\s*X\((\w+), "([^"]+)"', PRESETS.read_text(), re.MULTILINE)
    rows = dict(re.findall(r'^\s*\[STRESS_(\w+)\] = \{\w+, \d+, (\w+),', TABLE.read_text(), re.MULTILINE))
    return {key: int(rows[name]) if rows[name].isdigit() else read_define(PRESETS, rows[name])
            for name, key in keys}


BUTTERFLY_CLUSTERS_DRAWN = {"butterflies": 86, "few-butterflies": 12}
MOTION_DT = 20
MOTION_FRAMES = 30
MOTION_TOLERANCE = 2e-6


class StressCorners(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = pathlib.Path(tempfile.mkdtemp(prefix="stress_corners_"))
        run, cls.binary = host_render.run(cls.out, "--build-only")
        cls.build_output = run.stdout + run.stderr
        cls.built = run.returncode == 0
        probe = cls.out / "flap_constants.c"
        probe.write_text('#include <stdio.h>\n#include "apps/render_lab/stress/scene_stress.h"\n#include "math/scalar/units.h"\n'
                         'int main(void) { printf("%.9g %.9g %.9g %.9g %.9g\\n", (double)STRESS_FLAP_AMPLITUDE, '
                         '(double)STRESS_FLAP_BIAS, (double)STRESS_FLAP_HZ, (double)STRESS_FLAP_PHASE_STEP, '
                         '(double)MATH_MS_PER_S); }\n')
        root = HERE.parents[5]
        includes = [root / "launcher/main"] + list((root / "launcher/packages").glob("*/include"))
        binary = cls.out / "flap_constants"
        subprocess.run([host_cc(), str(probe), "-o", str(binary),
                        *["-I" + str(path) for path in includes]], check=True, capture_output=True)
        cls.amplitude, cls.bias, cls.hz, cls.phase_step, cls.ticks_per_second = map(
            float, subprocess.check_output([str(binary)], text=True).split())

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def setUp(self):
        self.assertTrue(self.built, self.build_output[-2000:])

    def frames(self, key, tag=None, dt=None, frames=FRAMES + 1, transforms=False, console=()):
        """Raster frames before a still camera, optionally with each update's local transforms."""
        dump = self.out / (tag or "still-" + key)
        dump.mkdir()
        poses = dump / "transforms.csv"
        options = (["--dt", str(dt)] if dt is not None else []) + (["--transforms", str(poses)] if transforms else [])
        options += [option for command in console for option in ("--console", command)]
        run = subprocess.run([str(self.binary), "--quarter", "1", "--no-hud", "--frames", str(frames), "--scene",
                              "stress-" + key, "--camera", "still", "--dump", str(dump), *options, "-o",
                              str(dump / "last.bmp")], capture_output=True, text=True, timeout=120)
        self.assertEqual(0, run.returncode, run.stderr[-2000:])
        if transforms:
            with poses.open(newline="") as stream:
                rows = {(int(row["frame"]), row["name"]): row for row in csv.DictReader(stream)}
            return rows, [(dump / ("%04d.raw" % i)).read_bytes() for i in range(frames)]
        return [(dump / ("%04d.raw" % i)).read_bytes() for i in (frames - 2, frames - 1)]

    def counts(self, key, *options, frames=FRAMES):
        """Every frame's counts of preset `key`, by name, the last frame last."""
        rows = self.out / (key + ".csv")
        run = subprocess.run([str(self.binary), "--quarter", "1", "--no-hud", "--frames", str(frames), *options, "--scene",
                              "stress-" + key, "--view", "cull", "--counts", str(rows), "-o",
                              str(self.out / (key + ".bmp"))], capture_output=True, text=True, timeout=120)
        self.assertEqual(0, run.returncode, run.stderr[-2000:])
        with rows.open(newline="") as f:
            return [{name: int(value) for name, value in row.items()} for row in csv.DictReader(f)]

    def last_counts(self, key, *options):
        """The last frame's counts of preset `key`, by name."""
        return self.counts(key, *options)[-1]

    def test_skinned_contacts_darken_each_receiver(self):
        for key in ("skinned-sh-blobs", "skinned-feet-blobs", "skinned-silhouette-blobs", "skinned-silhouette-warped"):
            with self.subTest(preset=key):
                image = self.out / (key + "-contacts.bmp")
                run = subprocess.run([str(self.binary), "--quarter", "1", "--no-hud", "--frames", str(FRAMES),
                                      "--scene", "stress-" + key, "--camera", "still",
                                      "--dt", str(CONTACT_VIEW_DT_MS),
                                      *[arg for step in CONTACT_VIEW_STEERING for arg in ("--console", step)],
                                      "-o", str(image)],
                                     capture_output=True, text=True, timeout=120)
                self.assertEqual(0, run.returncode, run.stderr[-2000:])
                width, height, pixels = read_bmp(image)
                colours = collections.Counter(pixel for row in pixels for pixel in row if pixel != pixels[0][0])
                ground = colours.most_common(1)[0][0]
                half = gfx_color.expand(*(channel // (2 * step) for channel, step in zip(ground, gfx_color.STEP)))
                receiver_rows = [y for y, line in enumerate(pixels) if ground in line]
                top, bottom = receiver_rows[0], receiver_rows[-1] + 1
                copies = preset_copies()[key]
                columns = math.ceil(math.sqrt(copies))
                rows = math.ceil(copies / columns)
                for copy in range(copies):
                    col, row = copy % columns, copy // columns
                    region = [pixel for line in pixels[top + row * (bottom - top) // rows:top + (row + 1) * (bottom - top) // rows]
                              for pixel in line[col * width // columns:(col + 1) * width // columns]]
                    self.assertGreater(region.count(half), 0, f"{key} copy {copy}: no halved receiver pixel")

    def test_path_replay_moves_wings_holds_and_repeats(self):
        rows, _ = self.frames("few-butterflies", tag="path-replay", dt=MOTION_DT, frames=7, transforms=True,
                                   console=("0:pose 0", f"1:pose {MOTION_DT}", "3:pose play", f"5:pose {MOTION_DT}"))
        wing = "slot_00_wing_l"
        rotations = [tuple(rows[i, wing][key] for key in ("qx", "qy", "qz", "qw")) for i in range(7)]
        self.assertNotEqual(rotations[0], rotations[1])
        self.assertEqual(rotations[1], rotations[2])
        self.assertNotEqual(rotations[2], rotations[3])
        self.assertEqual(rotations[1], rotations[5])
        # Empty-depth colour is scratch; the upscaler supplies the background.
        frames = []
        for index in range(7):
            colour, depth = read_dump(self.out / "path-replay" / ("%04d.raw" % index))
            frames.append((depth.tobytes(), colour[depth != R3D_DEPTH_EMPTY].tobytes()))
        # The host composes before its provider runs the console command.
        self.assertNotEqual(frames[1], frames[2])
        self.assertEqual(frames[2], frames[3])
        self.assertEqual(frames[2], frames[6])

    def test_shipped_update_raises_mirrored_wings_with_limits_and_copy_phases(self):
        rows, _ = self.frames("few-butterflies", tag="wing-tips", dt=MOTION_DT, frames=MOTION_FRAMES, transforms=True)
        vertices = [[float(value) for value in line.split()[1:]]
                    for line in (BUTTERFLY_DIR / "butterfly_wing.obj").read_text().splitlines()
                    if line.startswith("v ")]
        authored = max(vertices, key=lambda v: abs(v[0]))
        # The OBJ importer changes handedness on z.
        tip = [authored[0], authored[1], -authored[2]]
        for frame in range(MOTION_FRAMES):
            tips = []
            for copy in range(preset_copies()["few-butterflies"]):
                angle = self.amplitude * math.sin(2 * math.pi * self.hz * (frame + 1) * MOTION_DT / self.ticks_per_second
                                                   + self.phase_step * copy) + self.bias
                left = qrot(tuple(float(rows[frame, f"slot_{copy:02d}_wing_l"][k])
                                  for k in ("qx", "qy", "qz", "qw")), tip)
                right = qrot(tuple(float(rows[frame, f"slot_{copy:02d}_wing_r"][k])
                                   for k in ("qx", "qy", "qz", "qw")), tip)
                self.assertAlmostEqual(-left[0], right[0], delta=MOTION_TOLERANCE)
                self.assertAlmostEqual(left[1], right[1], delta=MOTION_TOLERANCE)
                self.assertAlmostEqual(left[2], right[2], delta=MOTION_TOLERANCE)
                self.assertAlmostEqual(-tip[0] * math.sin(angle), left[2], delta=MOTION_TOLERANCE)
                self.assertLessEqual(abs(math.atan2(left[2], -left[0])), (self.amplitude + self.bias) + MOTION_TOLERANCE)
                if angle > 0:
                    self.assertGreater(left[2], 0)
                tips.append(left[2])
            self.assertGreater(max(tips) - min(tips), MOTION_TOLERANCE)

    def test_update_time_is_accumulated_dt_and_same_steps_repeat_frames(self):
        rows, frames = self.frames("few-butterflies", tag="clock-first", dt=MOTION_DT, frames=4, transforms=True)
        again_rows, again_frames = self.frames("few-butterflies", tag="clock-again", dt=MOTION_DT, frames=4, transforms=True)
        self.assertEqual(rows, again_rows)
        self.assertEqual(frames, again_frames)
        later_rows, later_frames = self.frames("few-butterflies", tag="clock-later", dt=MOTION_DT * 2, frames=4, transforms=True)
        self.assertNotEqual(frames[-1], later_frames[-1])
        # Splitting the same elapsed time into twice as many updates keeps the pose.
        split_rows, _ = self.frames("few-butterflies", tag="clock-split", dt=MOTION_DT // 2, frames=8, transforms=True)
        for name in ("slot_00_wing_l", "slot_00_wing_r"):
            for component in ("qx", "qy", "qz", "qw"):
                self.assertAlmostEqual(float(rows[3, name][component]), float(split_rows[7, name][component]),
                                       delta=MOTION_TOLERANCE)
        # Zero elapsed time stays at the prescribed bias, independent of draw count.
        zero, _ = self.frames("few-butterflies", tag="clock-zero", dt=0, frames=3, transforms=True)
        row = zero[2, "slot_00_wing_l"]
        self.assertAlmostEqual(math.sin(self.bias / 2), float(row["qy"]), delta=MOTION_TOLERANCE)
        self.assertNotEqual(rows[3, "slot_00_wing_l"], later_rows[3, "slot_00_wing_l"])

    def test_every_preset_is_held_here(self):
        self.assertEqual({"open", "stack", "tiny-copies", "big-tris", "per-frame", "two-sided", "noisy-tiny", "mix",
                          "static-copies", "uniform-mid", "uniform-full", "card", "culled", "near",
                          "few-copies", "carved-near", "carved-far", "textured", "textured-far", "record", "foliage-near",
                          "foliage-near-opaque", "foliage-far", "foliage-far-opaque", "skinned-sh-blobs",
                          "skinned-feet-blobs", "skinned-silhouette-blobs", "skinned-silhouette-warped", "skinned-blob-warped", "contact-blobs",
                          "butterflies", "few-butterflies", "cull-setup", "baked-tree", "small-grain", "small-dither",
                          "small-off", "stack-skip", "carved-blend"}, set(preset_copies()))

    def test_the_baked_ring_is_culled_through_its_tree(self):
        """The ring alone, one mesh and no copy, culled through its tree at every pose of the turn."""
        frames = self.counts("baked-tree", frames=SWEEP_FRAMES)
        for c in frames:
            self.assertEqual(1, c["transform_batches"], "the ring's own pack shows one mesh")
            self.assertGreater(c["nodes_cut"], 0, "part of the ring is always behind the camera")
            self.assertGreater(c["clusters_kept"], 0, "part of the ring is always in view")
        self.assertGreater(len({c["clusters_kept"] for c in frames}), 1, "the camera turns: the kept set changes")

    def assert_all_in_view(self, c):
        """Nothing culled: every cluster tested is kept, and no node is cut."""
        self.assertEqual(0, c["nodes_cut"])
        self.assertEqual(c["clusters_tested"], c["clusters_kept"])

    def assert_draws_every_triangle(self, key):
        c = self.last_counts(key)
        self.assertGreater(c["triangles_submitted"], 0)
        self.assertEqual(c["triangles_submitted"], c["triangles_covering"])

    def test_static_copies_keep_the_pack_mesh(self):
        c = self.last_counts("static-copies")
        self.assertGreater(c["clusters_kept"], 0)
        self.assertEqual(0, c["triangles_facing_away"])
        self.assertEqual(c["triangles_submitted"], c["triangles_covering"])
        self.assertEqual(c["triangles_submitted"], c["clusters_kept"])
        self.assertEqual(c["vertices_transformed"], STATIC_VERTICES * c["clusters_kept"])
        self.assertGreater(self.last_counts("open")["triangles_submitted"], c["triangles_submitted"])

    def test_uniform_area_spans_water_without_shaded_pixels(self):
        middle = self.last_counts("uniform-mid", "--size", "368x224")
        full = self.last_counts("uniform-full", "--size", "368x224")
        for c in (middle, full):
            self.assertGreater(c["pixels_uniform"], 0)
            self.assertEqual(0, c["pixels_shaded"])
            self.assertEqual(0, c["pixels_face"])
            self.assertGreater(c["sized_uniform_more"], 0)
        self.assertGreater(full["pixels_uniform"], WATER_UNIFORM_PIXELS)
        self.assertGreater(middle["pixels_uniform"], EXISTING_UNIFORM_PIXELS)
        self.assertLess(middle["pixels_uniform"], WATER_UNIFORM_PIXELS)

    def test_upscale_counts_describe_actual_compose(self):
        half = self.last_counts("uniform-mid")
        wide = self.last_counts("uniform-mid", "--size", "368x224")
        self.assertEqual(184 * 224, half["render_width"] * half["render_height"])
        self.assertEqual(368 * 448, half["output_width"] * half["output_height"])
        self.assertEqual(368 * 224, wide["render_width"] * wide["render_height"])
        self.assertEqual(368 * 448, wide["output_width"] * wide["output_height"])
        from render.cull_counts import UPSCALE_PATHS
        self.assertEqual("kept", UPSCALE_PATHS[wide["upscale_path"]])

    def test_mix_fills_every_uniform_and_shaded_size_bin(self):
        c = self.last_counts("mix")
        for mode in ("uniform", "shaded"):
            for size in ("zero", "one", "two_to_four", "more"):
                self.assertGreater(c[f"sized_{mode}_{size}"], 0, f"{mode}/{size}")
        self.assertEqual(c["triangles_set_up"], sum(value for name, value in c.items() if name.startswith("sized_")))

    def test_open_hides_and_culls_nothing(self):
        c = self.last_counts("open")
        self.assertGreater(c["clusters_drawn"], 0)
        self.assertEqual(0, c["clusters_hidden"])
        self.assert_all_in_view(c)

    def test_stack_hides_most_of_what_it_keeps(self):
        c = self.last_counts("stack")
        self.assertGreater(c["clusters_drawn"], 0)
        self.assertGreater(c["clusters_hidden"], c["clusters_drawn"])

    def test_tiny_copies_are_all_in_view(self):
        c = self.last_counts("tiny-copies")
        self.assertGreaterEqual(c["clusters_drawn"], preset_copies()["tiny-copies"])
        self.assert_all_in_view(c)

    def test_cull_setup_walks_almost_nothing(self):
        """One copy of a few clusters: the cull's walk is a few nodes, so what it costs is its setup."""
        self.assertEqual(1, preset_copies()["cull-setup"])
        c = self.last_counts("cull-setup")
        self.assert_all_in_view(c)
        tiny = self.last_counts("tiny-copies")
        self.assertLessEqual(c["clusters_kept"], tiny["clusters_kept"] / preset_copies()["tiny-copies"])

    def test_few_copies_give_each_core_one_dense_copy(self):
        """Two copies, each many clusters: the cull's split puts one whole copy on each core."""
        copies = preset_copies()["few-copies"]
        self.assertEqual(2, copies)
        c = self.last_counts("few-copies")
        self.assert_all_in_view(c)
        tiny = self.last_counts("tiny-copies")
        tiny_clusters_per_copy = tiny["clusters_kept"] / preset_copies()["tiny-copies"]
        tiny_triangles_per_copy = tiny["triangles_submitted"] / preset_copies()["tiny-copies"]
        # Dense: each copy holds a multiple of what a tiny copy does, in clusters and in triangles.
        self.assertGreater(c["clusters_kept"] / copies, DENSE_FACTOR * tiny_clusters_per_copy)
        self.assertGreater(c["triangles_submitted"] / copies, DENSE_FACTOR * tiny_triangles_per_copy)
        # Equal weight: the same shape twice, so every total splits evenly between the two copies.
        for name in ("clusters_kept", "triangles_submitted", "vertices_transformed"):
            self.assertEqual(0, c[name] % copies, name)

    def test_carved_copies_draw_a_coarser_level_far_than_near(self):
        """The carved prop's copies each pick a level: far copies draw fewer triangles apiece than near ones."""
        near = self.last_counts("carved-near")
        far = self.last_counts("carved-far")
        for c in (near, far):
            self.assertGreater(c["triangles_submitted"], 0)
        per_copy = {key: c["triangles_submitted"] / preset_copies()[key]
                    for key, c in (("carved-near", near), ("carved-far", far))}
        self.assertLess(per_copy["carved-far"], per_copy["carved-near"])

    def test_two_sided_skips_nothing_facing_away(self):
        """A closed shape drawn from both sides: the one-sided spheres skip their far half, these cubes none."""
        self.assertGreater(self.last_counts("tiny-copies")["triangles_facing_away"], 0)
        c = self.last_counts("two-sided")
        self.assertGreater(c["triangles_submitted"], 0)
        self.assertEqual(0, c["triangles_facing_away"])
        self.assertGreater(c["clusters_hidden"], 0)

    def test_noisy_tiny_copies_are_all_in_view(self):
        c = self.last_counts("noisy-tiny")
        self.assertGreaterEqual(c["clusters_drawn"], preset_copies()["noisy-tiny"])
        self.assert_all_in_view(c)

    def test_big_tris_draws_every_triangle_it_submits(self):
        self.assert_draws_every_triangle("big-tris")

    def test_per_frame_draws_the_whole_wave(self):
        self.assert_draws_every_triangle("per-frame")

    def test_butterflies_keep_every_part_in_view_and_submit_every_triangle(self):
        parts = len(BUTTERFLY_PARTS)
        for key, drawn in BUTTERFLY_CLUSTERS_DRAWN.items():
            with self.subTest(key=key):
                copies = preset_copies()[key]
                c = self.last_counts(key)
                self.assert_all_in_view(c)
                self.assertEqual(copies * parts, c["transform_batches"])
                self.assertEqual(drawn, c["clusters_drawn"])
                self.assertEqual(copies * BUTTERFLY_TRIANGLES, c["triangles_submitted"])
                self.assertEqual(0, c["triangles_behind"])
                self.assertEqual(0, c["triangles_clipped"])

    def test_wave_and_hinges_move_before_a_still_camera(self):
        for key in ("per-frame", "butterflies", "few-butterflies"):
            with self.subTest(key=key):
                before, after = self.frames(key)
                self.assertNotEqual(before, after)
        before, after = self.frames("stack")
        self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
