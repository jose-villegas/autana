"""plane_tilt.py: a tilt keeps the t line in the plane through the eye and the
line, turns it by the angle asked, adds to an earlier tilt, and refuses a clip
whose plane moves."""

import math
import pathlib
import sys
import tempfile
import unittest

LAUNCHER = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(LAUNCHER / "tools"))
sys.path.insert(0, str(LAUNCHER / "tools" / "boot_anim"))

from gltf import gltf_write  # noqa: E402
import plane_tilt  # noqa: E402

EYE = (0.0, 0.0, 10.0)
ORIGIN = (-2.0, 8.0, 0.0)
START, END = 1.0, 2.0
TILT_DEG = 30.0
ANGLE_TOLERANCE_DEG = 0.01


def quat(axis, degrees):
    half = math.radians(degrees) / 2
    axis = plane_tilt.unit(axis)
    return tuple(c * math.sin(half) for c in axis) + (math.cos(half),)


def clip(end_turn_axis):
    """A camera at EYE and a space at ORIGIN whose rotation turns 45 degrees
    about `end_turn_axis` between START and END."""
    rest = quat((1.0, 0.0, 0.0), 0.0)
    nodes = [{"name": "camera", "translation": EYE}, {"name": "space", "translation": ORIGIN}]
    channels = [
        {"node": 0, "path": "translation", "interpolation": "LINEAR", "times": [0.0, END], "values": [EYE, EYE]},
        {"node": 1, "path": "translation", "interpolation": "LINEAR", "times": [0.0, END],
         "values": [ORIGIN, ORIGIN]},
        {"node": 1, "path": "rotation", "interpolation": "LINEAR", "times": [0.0, START, END],
         "values": [rest, rest, quat(end_turn_axis, 45.0)]},
    ]
    return gltf_write.build_glb(nodes, [{"name": "boot_motion", "channels": channels}])


def line_at(data, t):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "clip.glb"
        path.write_bytes(data)
        _, names, _, channels = plane_tilt.read_clip(path)
        return plane_tilt.qrot(plane_tilt.sampler(names, channels)("space", "rotation", t), plane_tilt.T_AXIS)


def tilted(data, degrees):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "clip.glb"
        path.write_bytes(data)
        return plane_tilt.tilt(path, degrees, START, END)


def screen_heading_deg(line):
    """The line's direction on screen: the camera sits at EYE looking down -z
    with no turn, so a point projects to (x, y) / -z of its offset from EYE."""
    def project(p):
        x, y, z = (a - b for a, b in zip(p, EYE))
        return (x / -z, y / -z)
    near = project(ORIGIN)
    far = project(tuple(o + d for o, d in zip(ORIGIN, line)))
    return math.degrees(math.atan2(far[1] - near[1], far[0] - near[0]))


def angle_deg(a, b):
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))))


class PlaneTiltTests(unittest.TestCase):
    def test_the_line_keeps_its_place_on_screen_and_turns_by_the_angle_asked(self):
        data, _ = tilted(clip(plane_tilt.T_AXIS), TILT_DEG)
        heading = screen_heading_deg(line_at(data, START))
        steps = int(round((END - START) / plane_tilt.CHECK_STEP_S))
        for k in range(steps + 1):
            t = START + k * (END - START) / steps
            self.assertAlmostEqual(screen_heading_deg(line_at(data, t)), heading, delta=plane_tilt.MAX_ERROR_DEG,
                                   msg="the t line turned on screen at %.2f s" % t)
        self.assertAlmostEqual(angle_deg(line_at(data, START), line_at(data, END)), TILT_DEG,
                               delta=ANGLE_TOLERANCE_DEG)

    def test_a_second_tilt_adds_to_the_first(self):
        once, _ = tilted(clip(plane_tilt.T_AXIS), TILT_DEG)
        twice, _ = tilted(once, TILT_DEG)
        self.assertAlmostEqual(angle_deg(line_at(twice, START), line_at(twice, END)), 2 * TILT_DEG,
                               delta=ANGLE_TOLERANCE_DEG)

    def test_a_clip_whose_line_leaves_the_plane_is_refused(self):
        with self.assertRaises(SystemExit) as raised:
            tilted(clip((0.0, 0.0, 1.0)), TILT_DEG)
        self.assertIn("moves between", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
