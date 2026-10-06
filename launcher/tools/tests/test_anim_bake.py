"""Bakes a glTF built here, samples the baked clip in C through
anim/track_host.py (the device's sampler over the clip's TRCK entry), and
holds it to the Python sampler in gltf/gltf_read.py: every interpolation,
quaternion slerp, loop and clamp on one shared timeline, and properties
reached by a KHR_animation_pointer. The scene is invented in this test, so
nothing here depends on one an app ships. Skipped where there is no C
compiler or no sh."""

import pathlib
import shutil
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "anim"))
sys.path.insert(0, str(TOOLS / "tests"))

import bake_tracks  # noqa: E402
from anim import track_host, tracks_asset  # noqa: E402
from anim_probe import channel, probe_glb  # noqa: E402
from gltf import gltf_read, gltf_write  # noqa: E402
from tests.test_track_host import has_compiler  # noqa: E402

TOLERANCE = 2e-5
EVERY_MS = 37
UNTIL_MS = 4200
NAME = "probe"


def bake_into(directory, glb_bytes):
    glb = pathlib.Path(directory) / "probe.glb"
    glb.write_bytes(glb_bytes)
    bake_tracks.main([str(glb), "--animation", "clip", "--name", NAME, "--out-dir", str(directory)])
    (pathlib.Path(directory) / (NAME + tracks_asset.SUFFIX)).write_text('source = "probe.glb"\nanimation = "clip"\n')
    return glb


@unittest.skipUnless(has_compiler(), "needs sh and a C compiler")
class BakeRoundTripTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = pathlib.Path(tempfile.mkdtemp())
        glb = bake_into(cls.dir, probe_glb())
        cls.document, cls.binary = gltf_read.load_glb(str(glb))
        animation = cls.document["animations"][0]
        cls.animation = gltf_read.read_animation(cls.document, cls.binary, animation)
        cls.channels = {tracks_asset.channel_name(cls.document, c): c for c in cls.animation}
        cls.duration_ms = round(gltf_read.animation_duration(cls.animation) * 1000)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def run_host(self, *flags):
        return track_host.sample(self.dir / (NAME + tracks_asset.SUFFIX), flags)

    def sampled(self, *flags):
        rows = []
        for line in self.run_host("--every", EVERY_MS, "--until", UNTIL_MS, *flags).splitlines():
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
        start = 4000000000 - 4000000000 % self.duration_ms + 1200
        for line in self.run_host("--from", start, "--every", 7, "--until", start + 60).splitlines():
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

    def test_the_clip_carries_its_duration_and_the_tracks_and_names_are_separate_tables(self):
        source = (self.dir / (NAME + "_tracks_generated.c")).read_text()
        self.assertIn("const anim_clip_t probe_clip = {3000};", source)
        self.assertIn("const anim_track_t* const probe_tracks[]", source)
        self.assertIn("const int probe_track_count = 8;", source)
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
        self.assertIn("probe_clip = {2000};", source)


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
