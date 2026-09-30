"""Bakes a glTF built here, samples the baked tracks in C, and holds them to
the Python sampler in r3d/gltf_skin.py: every interpolation, quaternion
slerp, loop and clamp, and a property reached by a KHR_animation_pointer.
The scene is invented in this test, so nothing here depends on one an app
ships. Skipped where there is no C compiler or no sh."""

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
import gltf_write  # noqa: E402
from r3d import gltf_skin  # noqa: E402

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


def probe_glb():
    nodes = [{"name": "lamp"}, {"name": "arm"}]
    channels = [
        channel(0, "translation", [0.0, 1.0, 2.5], [(0, 0, 0), (4, -2, 1), (1, 1, 1)]),
        # The second pair of keys is in the opposite hemisphere: slerp must take the short way.
        channel(0, "rotation", [0.0, 1.0, 2.5],
                [turn((0, 1, 0), 10), turn((0, 1, 0), 170), tuple(-x for x in turn((0, 1, 0), 200))]),
        channel(1, "scale", [0.0, 1.0, 2.0], [(1, 1, 1), (2, 3, 4), (0.5, 0.5, 0.5)], "STEP"),
        channel(1, "translation", [0.5, 1.5, 3.0],
                cubic_rows([(0, 0, 0), (2, 1, 0), (0, 3, 3)], [(1, 0, 0), (0, 2, 0), (-1, -1, 0)]), "CUBICSPLINE"),
        channel(1, "rotation", [0.5, 1.5, 3.0],
                cubic_rows([turn((1, 0, 0), 0), turn((1, 0, 0), 90), turn((1, 0, 0), 30)],
                           [(0.2, 0, 0, 0), (0, 0.3, 0, 0), (0, 0, 0.1, 0)]), "CUBICSPLINE"),
        channel(None, "pointer", [0.0, 0.8, 2.0], [(0.6,), (1.2,), (0.9,)],
                pointer="/cameras/0/perspective/yfov"),
    ]
    return gltf_write.build_glb(
        nodes, [{"name": "clip", "channels": channels}],
        cameras=[{"type": "perspective", "perspective": {"yfov": 0.6, "znear": 0.1}}])


def find_sh():
    return shutil.which("sh")


@unittest.skipUnless(find_sh() and any(shutil.which(c) for c in ("cc", "gcc", "clang")), "needs sh and a C compiler")
class BakeRoundTripTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = pathlib.Path(tempfile.mkdtemp())
        glb = cls.dir / "probe.glb"
        glb.write_bytes(probe_glb())
        bake_tracks.main([str(glb), "--animation", "clip", "--name", NAME, "--out-dir", str(cls.dir)])
        cls.document, cls.binary = gltf_skin.load_glb(str(glb))
        animation = cls.document["animations"][0]
        cls.channels = {
            bake_tracks.channel_name(cls.document, c): c
            for c in gltf_skin.read_animation(cls.document, cls.binary, animation)
        }

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
        c = self.channels[name]
        first, last = c["times"][0], c["times"][-1]
        local = t_ms / 1000.0
        if loop:
            local = math.fmod(local, last - first)
        return gltf_skin.sample_keys(c["times"], c["values"], first + local, c["interpolation"],
                                     c["path"] == "rotation")

    def assert_every_sample_matches(self, loop):
        rows = self.sampled(*(() if loop else ("--clamp",)))
        self.assertGreater(len(rows), 6 * (UNTIL_MS // EVERY_MS))
        for t_ms, name, got in rows:
            want = self.reference(name, t_ms, loop)
            for a, b in zip(got, want):
                self.assertAlmostEqual(a, b, delta=TOLERANCE, msg="%s at %d ms" % (name, t_ms))
        return {name for _, name, _ in rows}

    def test_every_interpolation_matches_the_python_sampler_looping(self):
        names = self.assert_every_sample_matches(loop=True)
        self.assertEqual(names, set(self.channels))

    def test_every_interpolation_matches_the_python_sampler_clamped_past_the_end(self):
        self.assert_every_sample_matches(loop=False)

    def test_a_clamped_track_holds_its_last_key_and_a_looped_one_wraps(self):
        end = {(t, n): v for t, n, v in self.sampled("--clamp")}
        last_ms = max(t for t, _ in end)
        last_key = self.channels["lamp/translation"]["values"][-1]
        for a, b in zip(end[(last_ms, "lamp/translation")], last_key):
            self.assertAlmostEqual(a, b, delta=TOLERANCE)
        looped = {(t, n): v for t, n, v in self.sampled()}
        period_ms = 2500
        early = min(t for t, _ in looped if t >= period_ms)
        wrapped = looped[(early, "lamp/translation")]
        again = self.reference("lamp/translation", early - period_ms, loop=False)
        for a, b in zip(wrapped, again):
            self.assertAlmostEqual(a, b, delta=TOLERANCE)

    def test_a_pointer_targeted_scalar_bakes_as_a_width_one_track(self):
        source = (self.dir / (NAME + "_tracks_generated.c")).read_text()
        self.assertIn('{"/cameras/0/perspective/yfov", &probe_cameras_0_perspective_yfov}', source)
        rows = [v for _, n, v in self.sampled() if n == "/cameras/0/perspective/yfov"]
        self.assertTrue(all(len(v) == 1 for v in rows))
        self.assertAlmostEqual(rows[0][0], 0.6, delta=TOLERANCE)

    def test_the_output_names_the_command_that_regenerates_it(self):
        source = (self.dir / (NAME + "_tracks_generated.c")).read_text()
        self.assertTrue(source.startswith("/*\n * GENERATED FILE - do not edit.\n"))
        self.assertIn("python tools/anim/bake_tracks.py", source)


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
