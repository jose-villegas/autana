"""Checks the timeline generator's rules against the shipped input, and that
the clip the firmware plays is the motion the generator checks."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

LAUNCHER = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(LAUNCHER / "tools"))

from anim import tracks_asset  # noqa: E402
from gltf import gltf_write  # noqa: E402
from r3d import build_pack  # noqa: E402

sys.path.insert(0, str(LAUNCHER / "tools" / "gen"))
import gen_boot_anim_timeline  # noqa: E402

GENERATOR = LAUNCHER / "tools" / "gen" / "gen_boot_anim_timeline.py"
TIMELINE = LAUNCHER / "main" / "boot" / "boot_anim_timeline.json"
MOTION = LAUNCHER / "main" / "boot" / "boot_anim_motion.glb"
CLIP = LAUNCHER / "main" / "boot" / "boot_anim_motion.anim.toml"


def motion_glb(space_scale):
    """A boot motion glTF whose space scales to `space_scale` and whose
    camera holds still, through 5.5 s."""
    def channel(node, path, values):
        return {"node": node, "path": path, "times": [0.0, 5.5], "values": values}
    identity = (0.0, 0.0, 0.0, 1.0)
    channels = []
    for node, scale in ((0, 1.0), (1, space_scale)):
        channels += [channel(node, "translation", [(0, 0, 0)] * 2),
                     channel(node, "rotation", [identity] * 2),
                     channel(node, "scale", [(1, 1, 1), (scale,) * 3])]
    return gltf_write.build_glb([{"name": "camera"}, {"name": "space"}],
                                [{"name": "boot_motion", "channels": channels}])


class TimelineGeneratorTests(unittest.TestCase):
    def generate(self, change=None, motion=None, config_change=None):
        config = json.loads(TIMELINE.read_text(encoding="utf-8"))
        if change is not None:
            change(config["timing"])
        if config_change is not None:
            config_change(config)
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "timeline.json"
            source.write_text(json.dumps(config), encoding="utf-8")
            glb = pathlib.Path(directory) / "motion.glb"
            glb.write_bytes(motion if motion is not None else MOTION.read_bytes())
            return subprocess.run(
                [sys.executable, str(GENERATOR), str(source), str(glb)],
                cwd=LAUNCHER, capture_output=True, text=True, check=False)

    def test_unknown_timing_key_fails_and_names_key(self):
        result = self.generate(lambda timing: timing.update(title_font=1))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown timing key(s): title_font", result.stderr)

    def test_nonpositive_half_fov_short_tan_is_rejected(self):
        for value in (0.0, -1.0):
            with self.subTest(value=value):
                result = self.generate(config_change=lambda config: config.update(
                    camera_half_fov_short_tan=value))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("camera_half_fov_short_tan must be greater than 0", result.stderr)

    def test_invalid_lens_settings_fail_with_named_messages(self):
        cases = (
            (lambda config: config.update(camera_half_fov_short_tan=float("nan")),
             "camera_half_fov_short_tan must be finite"),
            (lambda config: config.pop("camera_half_fov_short_tan"),
             "missing camera_half_fov_short_tan"),
            (lambda config: config.update(camera_focal=1.0),
             "unknown key: camera_focal"),
        )
        for change, message in cases:
            with self.subTest(message=message):
                result = self.generate(config_change=change)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_small_positive_half_fov_short_tan_is_accepted(self):
        result = self.generate(config_change=lambda config: config.update(camera_half_fov_short_tan=0.001))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_zero_scale_fails(self):
        result = self.generate(lambda timing: timing.update(title_scale=0))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("title_scale must be at least 1", result.stderr)

    def test_one_scale_passes(self):
        result = self.generate(lambda timing: timing.update(title_scale=1))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_two_scale_warns(self):
        result = self.generate(lambda timing: timing.update(title_scale=2))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("the 8x8 bitmap needs about 5x to read as a title", result.stderr)

    def test_three_scale_does_not_warn(self):
        result = self.generate(lambda timing: timing.update(title_scale=3))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("title_scale", result.stderr)

    def test_missing_scale_defaults_to_five(self):
        result = self.generate(lambda timing: timing.pop("title_scale"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("#define BOOT_ANIM_TITLE_SCALE 5", result.stdout)

    def test_keyframes_left_in_the_json_are_refused(self):
        result = self.generate(config_change=lambda config: config.update(keyframes=[]))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("keyframes no longer live in the timeline", result.stderr)

    def test_a_large_motion_scale_is_accepted_since_float_has_no_overflow_to_guard(self):
        result = self.generate(motion=motion_glb(space_scale=9.0))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_the_clip_boot_plays_is_the_motion_checked_here_in_a_pack_of_its_own(self):
        glb, animation = tracks_asset.load_source(CLIP)
        self.assertEqual(MOTION.resolve(), glb.resolve())
        self.assertEqual(gen_boot_anim_timeline.MOTION_ANIMATION, animation)
        # boot_anim_motion.c mounts the pack named after its one entry.
        clip = tracks_asset.clip_id(CLIP)
        self.assertEqual({clip: CLIP}, build_pack.pack_files([CLIP.parent])[clip])

if __name__ == "__main__":
    unittest.main()
