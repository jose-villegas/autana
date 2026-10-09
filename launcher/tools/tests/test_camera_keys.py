"""Camera keys baked from a .anim.toml and sampled by the firmware's track sampler."""
import pathlib
import math
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))

from anim import camera_keys, track_host, tracks_asset
from anim_probe import has_compiler


def keys_toml(keys):
    """A keys file's text: node camera, animation path."""
    tables = "".join("\n[[keys]]\nt = %r\neye = %r\nlook_at = %r\n" % (k["t"], k["eye"], k["look_at"]) for k in keys)
    return 'node = "camera"\nanimation = "path"\n' + tables


def clip_of(root, keys_text):
    """A path.anim.toml naming path.keys.toml, both written into `root`."""
    (root / "path.keys.toml").write_text(keys_text)
    clip = root / "path.anim.toml"
    clip.write_text('source = "path.keys.toml"\nanimation = "path"\n')
    return clip


class CameraKeysSourceTests(unittest.TestCase):
    def test_a_keys_source_bakes_as_the_glb_it_builds(self):
        keys = [dict(t=0, eye=[0, 0, 0], look_at=[0, 0, -1]), dict(t=2, eye=[1, 2, 3], look_at=[1, 2, 0])]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "path.glb").write_bytes(camera_keys.build(dict(node="camera", animation="path", keys=keys)))
            glb_clip = root / "glb.anim.toml"
            glb_clip.write_text('source = "path.glb"\nanimation = "path"\n')
            self.assertEqual(tracks_asset.bake(clip_of(root, keys_toml(keys))), tracks_asset.bake(glb_clip))

    def test_a_bad_keys_source_fails_the_bake_naming_the_file(self):
        overflow = [dict(t=0, eye=[0, 0, 0], look_at=[0, 0, -1]), dict(t=1, eye=[1e300, 0, 0], look_at=[0, 0, -1])]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for text in ('node = "camera"\nanimation = "path"\nkeys = []\n', "node = ", keys_toml(overflow)):
                with self.subTest(text=text), self.assertRaisesRegex(tracks_asset.TracksError, "path.keys.toml"):
                    tracks_asset.bake(clip_of(root, text))


@unittest.skipUnless(has_compiler(), "needs sh and a C compiler")
class CameraKeysTests(unittest.TestCase):
    def test_keys_forward_and_loop_survive_the_bake(self):
        keys = [dict(t=0, eye=[0, 0, 0], look_at=[0, 0, -1]),
                dict(t=1, eye=[2, 1, 0], look_at=[3, 2, 2]),
                dict(t=3, eye=[0, 0, 0], look_at=[0, 0, -1])]
        with tempfile.TemporaryDirectory() as directory:
            clip = clip_of(pathlib.Path(directory), keys_toml(keys))
            tracks, _ = tracks_asset.decode(tracks_asset.bake(clip))
            translation = next(t for t in tracks if t["name"] == "camera/translation")
            self.assertEqual(translation["values"][0], translation["values"][-1])
            rotation = next(t for t in tracks if t["name"] == "camera/rotation")
            for a, b in zip(rotation["values"], rotation["values"][1:]):
                self.assertGreaterEqual(sum(x * y for x, y in zip(a, b)), 0)
            rows = track_host.sample(clip, ["--every", "1000", "--until", "4000", "--poses", "camera",
                                            "100", "100", "0.62", "6"]).splitlines()
            poses = [[float(v) for v in row.split()[1:]] for row in rows if row.startswith("pose ")]
            for index, key in ((0, keys[0]), (1, keys[1]), (3, keys[2])):
                for got, want in zip(poses[index][:3], key["eye"]):
                    self.assertAlmostEqual(got, want, places=5)
                forward = [b - a for a, b in zip(key["eye"], key["look_at"])]
                length = math.sqrt(sum(v * v for v in forward))
                forward = [v / length for v in forward]
                for got, want in zip(poses[index][3:], forward):
                    self.assertAlmostEqual(got, want, places=5)
            self.assertEqual(poses[0], poses[3])

    def test_bad_keys_are_refused(self):
        key = dict(t=0, eye=[0, 0, 0], look_at=[0, 0, -1])
        for keys in ([key], [key, key], [key, dict(key, t=1, look_at=[0, 0, 0])],
                     [key, dict(key, t=1, look_at=[0, 1, 0])], [key, dict(key, t=float("nan"))]):
            with self.subTest(keys=keys), self.assertRaises(ValueError):
                camera_keys.build(dict(node="camera", animation="path", keys=keys))


if __name__ == "__main__":
    unittest.main()
