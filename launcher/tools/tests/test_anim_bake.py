"""Bakes a glTF built here, samples the baked clip in C, and holds it to the
Python sampler in gltf/gltf_read.py: every interpolation, quaternion slerp,
loop and clamp on one shared timeline, and properties reached by a
KHR_animation_pointer. The scene is invented in this test, so nothing here
depends on one an app ships. Skipped where there is no C compiler or no sh."""

import math
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "anim"))

import bake_tracks  # noqa: E402
from gltf import gltf_read, gltf_write  # noqa: E402

TOLERANCE = 2e-5
EVERY_MS = 37
UNTIL_MS = 4200
NAME = "probe"


def turn(axis, degrees):
    half = math.radians(degrees) / 2.0
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def cubic_rows(values, slopes):
    """glTF CUBICSPLINE rows: in-tangent, value, out-tangent per key."""
    rows = []
    for value, slope in zip(values, slopes):
        rows += [slope, value, slope]
    return rows


def channel(node, path, times, values, interpolation="LINEAR", pointer=None):
    return {"node": node, "path": path, "pointer": pointer, "interpolation": interpolation,
            "times": times, "values": values}


NODES = ("lamp", "arm", "hand")


def probe_channels(index):
    """The clip's channels, with each node at index[name] in the file."""
    lamp, arm, hand = (index[n] for n in NODES)
    return [
        channel(lamp, "translation", [0.0, 1.0, 2.5], [(0, 0, 0), (4, -2, 1), (1, 1, 1)]),
        # The second pair of keys is in the opposite hemisphere: slerp must take the short way.
        channel(lamp, "rotation", [0.0, 1.0, 2.5],
                [turn((0, 1, 0), 10), turn((0, 1, 0), 170), tuple(-x for x in turn((0, 1, 0), 200))]),
        channel(arm, "scale", [0.0, 1.0, 2.0], [(1, 1, 1), (2, 3, 4), (0.5, 0.5, 0.5)], "STEP"),
        # These start late and end last: the clip is 3 s, and these run 0.5 to 3.
        channel(arm, "translation", [0.5, 1.5, 3.0],
                cubic_rows([(0, 0, 0), (2, 1, 0), (0, 3, 3)], [(1, 0, 0), (0, 2, 0), (-1, -1, 0)]), "CUBICSPLINE"),
        channel(arm, "rotation", [0.5, 1.5, 3.0],
                cubic_rows([turn((1, 0, 0), 0), turn((1, 0, 0), 90), turn((1, 0, 0), 30)],
                           [(0.2, 0, 0, 0), (0, 0.3, 0, 0), (0, 0, 0.1, 0)]), "CUBICSPLINE"),
        channel(None, "pointer", [0.0, 0.8, 2.0], [(0.6,), (1.2,), (0.9,)],
                pointer="/cameras/0/perspective/yfov"),
        # A pointer to a rotation is a quaternion too.
        channel(None, "pointer", [0.0, 2.0], [turn((0, 0, 1), 0), turn((0, 0, 1), 200)],
                pointer="/nodes/%d/rotation" % hand),
        # A channel that never changes bakes to one key.
        channel(hand, "translation", [0.0, 3.0], [(7, 7, 7), (7, 7, 7)]),
    ]


def probe_glb(reordered=False):
    order = list(NODES)
    if reordered:
        order.reverse()
    index = {name: order.index(name) for name in NODES}
    cameras = [{"name": "lens", "type": "perspective", "perspective": {"yfov": 0.6, "znear": 0.1}}]
    return gltf_write.build_glb([{"name": n} for n in order],
                                [{"name": "clip", "channels": probe_channels(index)}], cameras=cameras)


def find_sh():
    return shutil.which("sh")


def bake_into(directory, glb_bytes):
    glb = pathlib.Path(directory) / "probe.glb"
    glb.write_bytes(glb_bytes)
    bake_tracks.main([str(glb), "--animation", "clip", "--name", NAME, "--out-dir", str(directory)])
    return glb


@unittest.skipUnless(find_sh() and any(shutil.which(c) for c in ("cc", "gcc", "clang")), "needs sh and a C compiler")
class BakeRoundTripTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = pathlib.Path(tempfile.mkdtemp())
        glb = bake_into(cls.dir, probe_glb())
        cls.document, cls.binary = gltf_read.load_glb(str(glb))
        animation = cls.document["animations"][0]
        cls.animation = gltf_read.read_animation(cls.document, cls.binary, animation)
        cls.channels = {bake_tracks.channel_name(cls.document, c): c for c in cls.animation}
        cls.duration_ms = round(gltf_read.animation_duration(cls.animation) * 1000)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def sampled(self, *flags):
        script = TOOLS / "anim" / "sample_tracks.sh"
        result = subprocess.run(
            [find_sh(), str(script), "--tracks", "%s:%s" % (self.dir / (NAME + "_tracks_generated.c"), NAME),
             "--every", str(EVERY_MS), "--until", str(UNTIL_MS), *flags],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = []
        for line in result.stdout.splitlines():
            t, name, *values = line.split(" ")
            rows.append((int(t), name, [float(v) for v in values]))
        return rows

    def reference(self, name, t_ms, loop):
        """The glTF clock: every channel reads the same second of the clip."""
        c = self.channels[name]
        clip_ms = t_ms % self.duration_ms if loop else min(t_ms, self.duration_ms)
        return gltf_gltf_sample(c, clip_ms / 1000.0)

    def assert_every_sample_matches(self, loop, *flags):
        rows = self.sampled(*flags)
        self.assertGreater(len(rows), 6 * (UNTIL_MS // EVERY_MS))
        for t_ms, name, got in rows:
            want = self.reference(name, t_ms, loop)
            for a, b in zip(got, want):
                self.assertAlmostEqual(a, b, delta=TOLERANCE, msg="%s at %d ms" % (name, t_ms))
        return {name for _, name, _ in rows}

    def test_every_interpolation_matches_the_python_sampler_looping(self):
        names = self.assert_every_sample_matches(True)
        self.assertEqual(names, set(self.channels))

    def test_every_interpolation_matches_the_python_sampler_clamped_past_the_end(self):
        self.assert_every_sample_matches(False, "--clamp")

    def test_channels_with_different_key_ranges_stay_on_one_timeline(self):
        self.assertEqual(self.duration_ms, 3000)
        early = self.channels["lamp/translation"]
        self.assertEqual(early["times"][-1], 2.5)
        looped = {(t, n): v for t, n, v in self.sampled()}
        # 3.2 s is 0.2 s into the second lap for every channel: the arm has not started its own range.
        arm = looped[(3182, "arm/translation")]
        self.assertEqual(arm, [0.0, 0.0, 0.0])

    def test_a_clamped_clip_holds_each_channels_last_key(self):
        rows = self.sampled("--clamp")
        last = max(t for t, _, _ in rows)
        self.assertGreater(last, self.duration_ms)
        end = {n: v for t, n, v in rows if t == last}
        for name, c in self.channels.items():
            key = c["values"][-1] if c["interpolation"] != "CUBICSPLINE" else c["values"][-2]
            for a, b in zip(end[name], key):
                self.assertAlmostEqual(a, b, delta=TOLERANCE, msg=name)

    def test_a_large_time_keeps_millisecond_resolution(self):
        script = TOOLS / "anim" / "sample_tracks.sh"
        start = 4000000000 - 4000000000 % self.duration_ms + 1200
        result = subprocess.run(
            [find_sh(), str(script), "--tracks", "%s:%s" % (self.dir / (NAME + "_tracks_generated.c"), NAME),
             "--from", str(start), "--every", "7", "--until", str(start + 60)],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        for line in result.stdout.splitlines():
            t, name, *values = line.split(" ")
            want = self.reference(name, int(t), True)
            for a, b in zip(map(float, values), want):
                self.assertAlmostEqual(a, b, delta=TOLERANCE, msg="%s at %s ms" % (name, t))

    def test_a_pointer_targeted_scalar_bakes_as_a_width_one_track_named_by_its_object(self):
        source = (self.dir / (NAME + "_tracks_generated.c")).read_text()
        self.assertIn('"lens/perspective/yfov"', source)
        self.assertIn("probe_lens_perspective_yfov", source)
        rows = [v for _, n, v in self.sampled() if n == "lens/perspective/yfov"]
        self.assertTrue(all(len(v) == 1 for v in rows))
        self.assertAlmostEqual(rows[0][0], 0.6, delta=TOLERANCE)

    def test_a_pointer_to_a_rotation_slerps(self):
        self.assertIn("hand/rotation", self.channels)
        source = (self.dir / (NAME + "_tracks_generated.c")).read_text()
        pointed = [line for line in source.splitlines() if line.startswith("const anim_track_t probe_hand_rotation ")]
        self.assertTrue(pointed[0].endswith("ANIM_LINEAR, 1};"), pointed)

    def test_the_output_names_the_command_that_regenerates_it(self):
        source = (self.dir / (NAME + "_tracks_generated.c")).read_text()
        self.assertTrue(source.startswith("/*\n * GENERATED FILE - do not edit.\n"))
        self.assertIn("python tools/anim/bake_tracks.py", source)

    def test_the_clip_carries_its_duration_and_the_names_are_a_separate_table(self):
        source = (self.dir / (NAME + "_tracks_generated.c")).read_text()
        self.assertIn("const anim_clip_t probe_clip = {probe_clip_tracks, ", source)
        self.assertIn(", 3000};", source)
        self.assertIn("const char* const probe_track_names[]", source)


def gltf_gltf_sample(c, seconds):
    return gltf_read.sample_keys(c["times"], c["values"], seconds, c["interpolation"], gltf_read.is_rotation(c))


class BakeBindingTest(unittest.TestCase):
    def symbols(self, glb):
        with tempfile.TemporaryDirectory() as directory:
            bake_into(directory, glb)
            source = (pathlib.Path(directory) / (NAME + "_tracks_generated.c")).read_text()
        return {line.split()[3] for line in source.splitlines() if line.startswith("const anim_track_t ")}

    def test_a_reexport_that_reorders_objects_binds_the_same_symbols(self):
        self.assertEqual(self.symbols(probe_glb()), self.symbols(probe_glb(reordered=True)))

    def test_a_channel_that_never_changes_is_one_key(self):
        glb = gltf_write.build_glb(
            [{"name": "n"}], [{"name": "clip", "channels": [
                channel(0, "translation", [0.0, 1.0, 2.0], [(3, 3, 3)] * 3),
                channel(0, "scale", [0.0, 2.0], [(1, 1, 1), (2, 2, 2)])]}])
        with tempfile.TemporaryDirectory() as directory:
            bake_into(directory, glb)
            source = (pathlib.Path(directory) / (NAME + "_tracks_generated.c")).read_text()
        self.assertIn("probe_n_translation = {probe_n_translation_times, probe_n_translation_values, 1, 3,", source)
        self.assertIn("probe_n_scale = {probe_n_scale_times, probe_n_scale_values, 2, 3,", source)
        self.assertIn(", 2000};", source)


class BakeRefusalTest(unittest.TestCase):
    def bake(self, channels):
        glb = gltf_write.build_glb([{"name": "n"}], [{"name": "clip", "channels": channels}])
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "bad.glb"
            path.write_bytes(glb)
            bake_tracks.main([str(path), "--animation", "clip", "--name", "bad", "--out-dir", tmp])

    def test_keys_out_of_order_are_refused(self):
        with self.assertRaises(SystemExit):
            self.bake([channel(0, "translation", [0.0, 2.0, 1.0], [(0, 0, 0)] * 3)])

    def test_a_channel_naming_neither_a_node_nor_a_pointer_is_refused(self):
        with self.assertRaises(SystemExit):
            self.bake([channel(None, "translation", [0.0, 1.0], [(0, 0, 0)] * 2)])


if __name__ == "__main__":
    unittest.main()
