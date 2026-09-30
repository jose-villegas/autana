"""The boot animation editor's keyframes and the glTF they become: an ease is
a cubic and reads back as the ease, a rotation that is not one slerp between
its keys follows the Euler path it had, and the keyframes return."""

import math
import pathlib
import sys
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "boot_anim"))

import boot_motion  # noqa: E402
from r3d import gltf_skin  # noqa: E402


def frame(ms, space_pos, space_rot, ease="linear", scale=1.0):
    still = {"pos": [0, 0, -10], "rot": [0, 0, 0], "scale": [1, 1, 1]}
    return {"ms": ms, "camera": still,
            "space": {"pos": space_pos, "rot": space_rot, "scale": [scale] * 3}, "ease": ease}


KEYFRAMES = [
    frame(0, [10, 0, 0], [-135, -45, 90]),
    frame(700, [-5, 0, 0], [-135, -45, 90], "ease_out"),
    frame(1500, [-5, 0, 0], [-135, 27, 90]),
    frame(2000, [-5, 2, 0], [-135, 27, 90], "ease_in"),
    frame(2400, [-2, 8, 0], [-180, -45, 0]),
    frame(3600, [-2, 8, 0], [-180, 0, 0], scale=0.6),
]


def tracks_of(glb):
    document, binary = gltf_skin.parse_glb(glb)
    channels = gltf_skin.read_animation(document, binary, document["animations"][0])
    return {(document["nodes"][c["node"]]["name"], c["path"]): c for c in channels}


def sample(channel, seconds):
    return gltf_skin.sample_keys(channel["times"], channel["values"], seconds,
                                 channel["interpolation"], channel["path"] == "rotation")


class BootMotionTests(unittest.TestCase):
    def test_an_ease_is_a_cubic_that_matches_the_ease_curve(self):
        tracks = tracks_of(boot_motion.keyframes_to_glb(KEYFRAMES))
        move = tracks[("space", "translation")]
        self.assertEqual(move["interpolation"], "CUBICSPLINE")
        for u in (0.1, 0.25, 0.5, 0.9):
            got = sample(move, 0.7 * u)[0]
            want = 10 + (-5 - 10) * boot_motion.ease_value("ease_out", u)
            self.assertAlmostEqual(got, want, delta=1e-4)
        for u in (0.1, 0.5, 0.9):
            got = sample(move, 1.5 + 0.5 * u)[1]
            want = 0 + 2 * boot_motion.ease_value("ease_in", u)
            self.assertAlmostEqual(got, want, delta=1e-4)

    def test_a_channel_with_no_easing_stays_linear(self):
        tracks = tracks_of(boot_motion.keyframes_to_glb(KEYFRAMES))
        self.assertEqual(tracks[("camera", "translation")]["interpolation"], "LINEAR")
        self.assertEqual(tracks[("space", "scale")]["interpolation"], "LINEAR")

    def test_a_segment_turning_about_several_axes_follows_the_euler_path(self):
        tracks = tracks_of(boot_motion.keyframes_to_glb(KEYFRAMES))
        turn = tracks[("space", "rotation")]
        self.assertGreater(len([t for t in turn["times"] if 2.0 < t < 2.4]), 5)
        for u in (0.13, 0.5, 0.77):
            euler = [a + (b - a) * u for a, b in zip((-135, 27, 90), (-180, -45, 0))]
            want = boot_motion.euler_to_quat(*euler)
            got = sample(turn, 1.5 + 0.0 + (2.4 - 2.0) * u + 0.5)
            self.assertLess(boot_motion.rotation_angle_degrees(got, want), 0.5)

    def test_a_segment_turning_about_one_axis_needs_no_extra_keys(self):
        turn = tracks_of(boot_motion.keyframes_to_glb(KEYFRAMES))[("space", "rotation")]
        self.assertEqual([t for t in turn["times"] if 0.7 < t < 1.5], [])

    def test_the_keyframes_come_back(self):
        back = boot_motion.glb_to_keyframes(boot_motion.keyframes_to_glb(KEYFRAMES))
        by_ms = {k["ms"]: k for k in back}
        for original in KEYFRAMES:
            got = by_ms[original["ms"]]
            for a, b in zip(got["space"]["pos"], original["space"]["pos"]):
                self.assertAlmostEqual(a, b, delta=1e-3)
            for a, b in zip(got["space"]["scale"], original["space"]["scale"]):
                self.assertAlmostEqual(a, b, delta=1e-3)
            self.assertEqual(got["ease"], original["ease"], original["ms"])
            same = boot_motion.rotation_angle_degrees(
                boot_motion.euler_to_quat(*got["space"]["rot"]),
                boot_motion.euler_to_quat(*original["space"]["rot"]))
            self.assertLess(same, 0.01)

    def test_a_rotation_reads_back_as_the_triple_nearest_the_last(self):
        q = boot_motion.euler_to_quat(-135, -45, 90)
        principal = boot_motion.quat_to_euler(q)
        near_authored = boot_motion.quat_to_euler(q, near=(-135, -40, 90))
        self.assertGreater(abs(principal[0] - -135), 1.0)
        for a, b in zip(near_authored, (-135, -45, 90)):
            self.assertAlmostEqual(a, b, places=3)

    def test_euler_and_quaternion_agree_on_the_convention(self):
        # The same numbers suite_r3d_trs.c pins, so C and Python read one convention.
        q = boot_motion.euler_to_quat(-135, -45, 90)
        for a, b in zip(q, (0.5, 0.70710678, -0.5, 0.0)):
            self.assertAlmostEqual(a, b, places=6)
        q = boot_motion.euler_to_quat(-180, -45, 0)
        for a, b in zip(q, (0.92387953, 0.0, -0.38268343, 0.0)):
            self.assertAlmostEqual(abs(a), abs(b), places=6)

    def test_every_matrix_from_euler_angles_is_a_rotation(self):
        for angles in ((10, 20, 30), (-135, -45, 90), (170, -80, 5)):
            m = boot_motion.euler_to_matrix(*angles)
            for i in range(3):
                for j in range(3):
                    dot = sum(m[i][k] * m[j][k] for k in range(3))
                    self.assertAlmostEqual(dot, 1.0 if i == j else 0.0, places=9)


if __name__ == "__main__":
    unittest.main()
