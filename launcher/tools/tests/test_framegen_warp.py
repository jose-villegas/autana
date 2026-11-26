"""Checks the frame generation warp (r3d/framegen/warp.py) reads a dump's depth
at the scale the frame was drawn with, not the camera's near plane."""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.framegen import warp
except ImportError:
    np = None

PANEL = (184, 224)
SIZE = (46, 56)
LENS = 0.6
# A wall straight ahead, drawn with a depth scale eight times the camera's near
# plane, as the renderer fits it to the nearest bound in view.
WALL = 400.0
DEPTH_NEAR = 48.0
STEP = 100.0


@unittest.skipIf(np is None, "numpy is not installed")
class DumpDepthScaleTests(unittest.TestCase):
    def dump(self, depth_near, depth):
        folder = tempfile.TemporaryDirectory(prefix="framegen_warp_")
        self.addCleanup(folder.cleanup)
        path = pathlib.Path(folder.name) / "0000.raw"
        height, width = depth.shape
        colour = np.zeros(depth.shape, dtype="<u2")
        path.write_bytes(warp.DUMP_HEADER.pack(width, height, depth_near) + colour.tobytes()
                         + depth.astype("<u2").tobytes())
        return path

    def test_a_step_toward_a_wall_reads_the_wall_at_the_scale_it_was_drawn(self):
        lens = warp.Lens(*PANEL, LENS, *SIZE)
        sx, _ = lens.rays()
        # A wall facing the camera at view depth WALL: every pixel stores its inverse.
        stored = np.full(sx.shape, round(DEPTH_NEAR * warp.R3D_DEPTH_NEAREST / WALL), dtype=np.uint16)
        colour, depth, depth_near = warp.read_dump(self.dump(DEPTH_NEAR, stored))
        self.assertEqual(DEPTH_NEAR, depth_near)
        _, moved, written = warp.warp(colour, depth, depth_near, lens, (0.0, 0.0, 0.0, 0.0, 0.0, 1.0),
                                      (0.0, 0.0, STEP, 0.0, 0.0, 1.0))
        centre = moved[SIZE[1] // 2, SIZE[0] // 2]
        self.assertTrue(written[SIZE[1] // 2, SIZE[0] // 2])
        self.assertAlmostEqual(WALL - STEP, depth_near * warp.R3D_DEPTH_NEAREST / centre, delta=0.1)


if __name__ == "__main__":
    unittest.main()
