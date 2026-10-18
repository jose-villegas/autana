"""Checks the timeline generator's rules against the shipped input, and that
the clip the firmware plays is the motion the generator checks."""

import json
import re
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

    def test_floor_settings_have_named_validation(self):
        positive = ("ripple_wavelength_m", "ripple_clock_s", "ripple_group_speed_m_s", "ripple_width_m", "ripple_dispersion_m_s", "ripple_source_radius_m", "crest_height_m")
        nonnegative = ("ripple_height_m", "swell_height_m")
        for key in positive + nonnegative:
            for value in (-1, float("nan"), float("inf")):
                with self.subTest(key=key, value=value):
                    result = self.generate(config_change=lambda c: c.update({key: value}))
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(key + " must be", result.stderr)
        for key in ("ripple_impact_ms", "ripple_calm_after_ms", "swell_in_ms"):
            result = self.generate(lambda t: t.update({key: -1}))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(key + " must be", result.stderr)
        for key in ("wavelength_m", "period_ms", "direction_deg", "weight"):
            result = self.generate(config_change=lambda c: c["swell"][0].update({key: float("nan")}))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("swell[0]." + key + " must be", result.stderr)

    def test_floor_timing_rejects_nonintegers_and_nonfinite_values(self):
        for key in ("ripple_impact_ms", "ripple_calm_after_ms", "swell_in_ms"):
            for value in (0.5, float("nan"), float("inf")):
                with self.subTest(key=key, value=value):
                    result = self.generate(lambda t: t.update({key: value}))
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(key + " must be", result.stderr)
        for key in ("swell_in_ms",):
            result = self.generate(lambda t: t.update({key: 0}))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(key + " must be positive", result.stderr)

    def test_floor_table_encoding_ranges_are_checked(self):
        cases = (
            ({"ripple_width_m": 0.01}, "ripple_width_m"),
            ({"ripple_group_speed_m_s": 1e30}, "ripple_group_speed_m_s"),
            ({"ripple_height_m": 8}, "ripple_height_m + weighted swell_height_m"),
            ({"grid_step_m": 100}, "grid_step_m * grid_bands"),
            ({"swell": []}, "swell must be a nonempty list"),
            ({"swell": [0]}, "swell[0] must be an object"),
            ({"swell": [dict(wavelength_m=4, period_ms=10, direction_deg=0, weight=1)]}, "swell[0].period_ms"),
        )
        for changes, message in cases:
            with self.subTest(changes=changes):
                result = self.generate(config_change=lambda c: c.update(changes))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_superseded_floor_keys_are_unknown(self):
        for key in ("ripple_speed_m_s", "ripple_rings", "white_height_m"):
            result = self.generate(config_change=lambda c: c.update({key: 1}))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unknown key: " + key, result.stderr)
        for key in ("ripple_decay_ms", "swell_start_ms", "grid_hue_ms", "grid_hue_spread", "grid_whiten_max", "mesh_dash", "grid_max", "grid_ceiling_max",
                    "grid_spokes", "grid_spoke_dash", "grid_spoke_start_ms", "grid_spoke_draw_ms",
                    "axis_extend_start_ms", "axis_extend_ms"):
            result = self.generate(lambda t: t.update({key: 1}))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unknown timing key(s): " + key, result.stderr)

    def test_packet_and_shading_settings_are_validated(self):
        keys = ("ripple_clock_s", "ripple_group_speed_m_s", "ripple_width_m",
                "ripple_dispersion_m_s", "ripple_source_radius_m", "crest_height_m")
        for key in keys:
            for value in (0, -1, float("nan"), float("inf")):
                with self.subTest(key=key, value=value):
                    result = self.generate(config_change=lambda c: c.update({key: value}))
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(key + " must be positive", result.stderr)
        for key, values in (("floor_grey", (-1, 0x1000000, 0.5)),
                            ("grid_edge_fade_fraction", (-1, 1, float("nan"))),
                            ("ripple_calm_fraction", (-1, 2, float("nan"))),
                            ("crest_colour_stride", (0, -2, 1.5, 5)),
                            ("grid_level", (0, -0.5, 1.5, float("nan"))),
                            ("far_m", (0, -1, 12, 1e6, float("nan"))),
                            ("far_grow_power", (0, -1, float("nan"))),
                            ("far_line_cells", (0, -1, 1.5)),
                            ("far_line_growth", (0, 1, -2, float("nan"))),
                            ("far_front", (-0.1, 1, float("nan"))),
                            ("far_front_steps", (0, -1, 2.5))):
            for value in values:
                result = self.generate(config_change=lambda c: c.update({key: value}))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(key, result.stderr)
        for value in (0, -1):
            result = self.generate(lambda t: t.update({"far_grow_ms": value}))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("far_grow_ms", result.stderr)
        for key in ("mesh_dash_on_px", "mesh_dash_off_px"):
            for value in (0, -1, 256, 0.5):
                result = self.generate(lambda t: t.update({key: value}))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(key, result.stderr)

    def test_generated_header_is_current(self):
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, (TIMELINE.parent / "boot_anim_timeline.h").read_text(encoding="utf-8"))

    def test_removed_floor_wave_settings_are_unknown(self):
        for key in ("wave_height_m", "wave_wavelength_m", "wave_period_ms"):
            result = self.generate(config_change=lambda c: c.update({key: 1}))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unknown key: " + key, result.stderr)
        for key in ("wave_in_ms", "wave_out_ms"):
            result = self.generate(lambda t: t.update({key: 1}))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unknown timing key(s): " + key, result.stderr)

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
            (lambda config: config.update(camera_half_fov_short_tan=float("inf")),
             "camera_half_fov_short_tan must be finite"),
            (lambda config: config.update(camera_half_fov_short_tan=float("-inf")),
             "camera_half_fov_short_tan must be finite"),
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

    def test_invalid_mesh_settings_fail_with_named_messages(self):
        cases = (
            (lambda timing: timing.update(mesh_cells=0), None, "mesh_cells must be positive and even"),
            (lambda timing: timing.update(mesh_cells=-2), None, "mesh_cells must be positive and even"),
            (lambda timing: timing.update(mesh_cells=47), None, "mesh_cells must be positive and even"),
            (lambda timing: timing.update(mesh_dash_on_px=256), None, "mesh_dash_on_px must fit an unsigned byte"),
        )
        for change, config_change, message in cases:
            with self.subTest(message=message):
                result = self.generate(change, config_change=config_change)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_the_mesh_ends_where_the_fade_does(self):
        def define(result, name):
            return int(re.search(r"#define %s (\d+)" % name, result.stdout).group(1))

        for cells in (24, 48):
            with self.subTest(cells=cells):
                result = self.generate(lambda timing: timing.update(mesh_cells=cells))
                self.assertEqual(result.returncode, 0, result.stderr)
                reach = define(result, "BOOT_ANIM_GRID_STEP_Q12") * define(result, "BOOT_ANIM_GRID_BANDS")
                self.assertAlmostEqual(define(result, "BOOT_ANIM_MESH_STEP_Q12") * (cells // 2), reach,
                                       delta=cells // 2)

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
