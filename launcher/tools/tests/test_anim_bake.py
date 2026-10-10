"""Bakes a glTF built here, samples the baked clip in C through
anim/track_host.py (the device's sampler over the clip's TRCK entry), and
holds it to the Python sampler in gltf/gltf_read.py: every interpolation,
quaternion slerp, loop and clamp on one shared timeline, and properties
reached by a KHR_animation_pointer. The scene is invented in this test, so
nothing here depends on one an app ships. Skipped where there is no C
compiler or no sh."""

import math
import pathlib
import shutil
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "anim"))
sys.path.insert(0, str(TOOLS / "tests"))

from anim import track_host, tracks_asset  # noqa: E402
from anim_probe import channel, has_compiler, probe_glb, skeleton_glb  # noqa: E402
from gltf import gltf_read, gltf_write  # noqa: E402

TOLERANCE = 2e-5
EVERY_MS = 37
UNTIL_MS = 4200
NAME = "probe"


def clip_into(directory, glb_bytes):
    """The NAME.anim.toml naming animation "clip" of `glb_bytes`, written beside it."""
    (pathlib.Path(directory) / "probe.glb").write_bytes(glb_bytes)
    clip = pathlib.Path(directory) / (NAME + tracks_asset.SUFFIX)
    clip.write_text('source = "probe.glb"\nanimation = "clip"\n')
    return clip


def baked(glb_bytes):
    """{name: track} and the duration of the TRCK entry baked from `glb_bytes`."""
    with tempfile.TemporaryDirectory() as directory:
        tracks, duration_ms = tracks_asset.decode(tracks_asset.bake(clip_into(directory, glb_bytes)))
    return {t["name"]: t for t in tracks}, duration_ms


class CameraFovTest(unittest.TestCase):
    def test_short_axis_tangent_at_square_landscape_and_portrait_aspects(self):
        angle = math.pi / 3
        for aspect in (1.0, 2.0, 0.5):
            with self.subTest(aspect=aspect):
                self.assertAlmostEqual(gltf_read.camera_half_fov_short_tan(angle, aspect),
                                       math.tan(angle / 2) * min(aspect, 1.0))
        self.assertAlmostEqual(gltf_read.camera_half_fov_short_tan(angle), math.tan(angle / 2))

    def test_cubic_camera_values_and_tangents_use_static_aspect(self):
        angles = (0.6, 1.2)
        tangents = (0.2, -0.3)
        for aspect in (1.0, 2.0, 0.5):
            rows = [(tangents[0],), (angles[0],), (tangents[1],),
                    (tangents[1],), (angles[1],), (tangents[0],)]
            channels = [channel(None, "pointer", [0.0, 1.0], rows, "CUBICSPLINE",
                                pointer="/cameras/0/perspective/yfov")]
            cameras = [{"type": "perspective", "perspective":
                        {"yfov": angles[0], "znear": 0.1, "aspectRatio": aspect}}]
            tracks, _ = baked(gltf_write.build_glb([{"name": "camera", "camera": 0}],
                              [{"name": "clip", "channels": channels}], cameras=cameras))
            values = tracks["camera:CAMR.half_fov_short_tan"]["values"]
            for key, angle in enumerate(angles):
                tangent = math.tan(angle / 2)
                factor = min(aspect, 1.0)
                self.assertAlmostEqual(values[key * tracks_asset.CUBIC_RUNS + 1][0],
                                       tangent * factor, delta=TOLERANCE)
                for offset in (0, 2):
                    index = key * tracks_asset.CUBIC_RUNS + offset
                    self.assertAlmostEqual(values[index][0],
                        rows[index][0] * factor * (1 + tangent * tangent) / 2, delta=TOLERANCE)


@unittest.skipUnless(has_compiler(), "needs sh and a C compiler")
class BakeRoundTripTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = pathlib.Path(tempfile.mkdtemp())
        clip_into(cls.dir, probe_glb())
        cls.document, cls.binary = gltf_read.load_glb(str(cls.dir / "probe.glb"))
        animation = cls.document["animations"][0]
        cls.animation = gltf_read.read_animation(cls.document, cls.binary, animation)
        cls.channels = {}
        for channel in cls.animation:
            name = tracks_asset.channel_name(cls.document, channel)
            if channel["pointer"]:
                aspect = cls.document["cameras"][0]["perspective"].get("aspectRatio", 1.0)
                channel = dict(channel, values=[(gltf_read.camera_half_fov_short_tan(v[0], aspect),)
                                               for v in channel["values"]])
            cls.channels[name] = channel
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
        return sample_channel(c, clip_ms / 1000.0)

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
        early = self.channels["lamp:TRNS.position"]
        self.assertEqual(early["times"][-1], 2.5)
        looped = {(t, n): v for t, n, v in self.sampled()}
        # 3.2 s is 0.2 s into the second lap for every channel: the arm has not started its own range.
        arm = looped[(3182, "arm:TRNS.position")]
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
        tracks, _ = baked(probe_glb())
        self.assertEqual(len(tracks["lamp:CAMR.half_fov_short_tan"]["values"][0]), 1)
        rows = [v for _, n, v in self.sampled() if n == "lamp:CAMR.half_fov_short_tan"]
        self.assertTrue(all(len(v) == 1 for v in rows))
        self.assertAlmostEqual(rows[0][0], gltf_read.camera_half_fov_short_tan(0.6), delta=TOLERANCE)

    def test_a_pointer_to_a_rotation_slerps(self):
        self.assertIn("hand:TRNS.rotation", self.channels)
        tracks, _ = baked(probe_glb())
        self.assertEqual((tracks["hand:TRNS.rotation"]["interpolation"], tracks["hand:TRNS.rotation"]["quaternion"]),
                         ("LINEAR", True))

    def test_the_entry_carries_the_clip_duration_and_every_channel(self):
        tracks, duration_ms = baked(probe_glb())
        self.assertEqual(duration_ms, 3000)
        self.assertEqual(set(tracks), set(self.channels))
        self.assertEqual(len(tracks), 8)


def sample_channel(c, seconds):
    return gltf_read.sample_keys(c["times"], c["values"], seconds, c["interpolation"], gltf_read.is_rotation(c))


@unittest.skipUnless(has_compiler(), "needs sh and a C compiler")
class SkeletonBakeTest(unittest.TestCase):
    def test_joint_paths_and_c_samples_match_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            clip = clip_into(directory, skeleton_glb())
            entry = tracks_asset.bake(clip)
            tracks, duration = tracks_asset.decode(entry)
            self.assertEqual([t["path"] for t in tracks], ["butt", "butt/spine"])
            self.assertTrue(all(t["root"] == tracks_asset.ROOT_SKELETON for t in tracks))
            by_name = {t["name"]: t for t in tracks}
            for line in track_host.sample(clip, ["--every", EVERY_MS, "--until", UNTIL_MS]).splitlines():
                time, name, *values = line.split()
                t = by_name[name]
                want = gltf_read.sample_keys(t["times"], t["values"], (int(time) % duration) / 1000,
                                             t["interpolation"], t["quaternion"])
                for a, b in zip(map(float, values), want):
                    self.assertAlmostEqual(a, b, delta=TOLERANCE)


class BakeBindingTest(unittest.TestCase):
    def test_a_reexport_that_reorders_objects_binds_the_same_names_to_the_same_keys(self):
        tracks, _ = baked(probe_glb())
        reordered, _ = baked(probe_glb(reordered=True))
        self.assertEqual(tracks, reordered)

    def test_a_channel_that_never_changes_is_one_key(self):
        tracks, duration_ms = baked(gltf_write.build_glb(
            [{"name": "n"}], [{"name": "clip", "channels": [
                channel(0, "translation", [0.0, 1.0, 2.0], [(3, 3, 3)] * 3),
                channel(0, "scale", [0.0, 2.0], [(1, 1, 1), (2, 2, 2)])]}]))
        self.assertEqual((len(tracks["n:TRNS.position"]["times"]), len(tracks["n:TRNS.position"]["values"][0])), (1, 3))
        self.assertEqual((len(tracks["n:TRNS.scale"]["times"]), len(tracks["n:TRNS.scale"]["values"][0])), (2, 3))
        self.assertEqual(duration_ms, 2000)


class BakeRefusalTest(unittest.TestCase):
    def bake(self, channels):
        baked(gltf_write.build_glb([{"name": "n"}], [{"name": "clip", "channels": channels}]))

    def test_keys_out_of_order_are_refused(self):
        with self.assertRaisesRegex(tracks_asset.TracksError, "strictly increasing"):
            self.bake([channel(0, "translation", [0.0, 2.0, 1.0], [(0, 0, 0)] * 3)])

    def test_a_channel_naming_neither_a_node_nor_a_pointer_is_refused(self):
        with self.assertRaisesRegex(tracks_asset.TracksError, "unsupported target"):
            self.bake([channel(None, "translation", [0.0, 1.0], [(0, 0, 0)] * 2)])


if __name__ == "__main__":
    unittest.main()
