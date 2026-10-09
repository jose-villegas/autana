"""Checks the FBX to glTF converter on data/skinned_probe.fbx, a bar skinned to
two bones and bent 90 degrees about X by one animation, exported from Blender
in centimetres (data/build_bent_bar_fbx.py builds it). Each check reads
the converted .glb the way the tools do, so a wrong unit, axis, weight or key
shows up as a wrong pose."""

import math
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from fbx import fbx_to_glb  # noqa: E402
from gltf import gltf_read  # noqa: E402
from r3d import gltf_mesh, gltf_skin  # noqa: E402

PROBE = pathlib.Path(__file__).resolve().parent / "data" / "skinned_probe.fbx"

BAR_HEIGHT = 2.0
BAR_HALF_WIDTH = 0.1
ELBOW = 1.0
BEND_ANIMATION = "rig|bend"
BEND_DURATION = 29 / 30  # frames 1 to 30 at 30 fps
BAR_TRIANGLES = 48  # a cube of 2 by 2 quads per face
MAX_INFLUENCES = 4
# The probe holds a hundredth of each vertex on five still bones; keeping four
# influences drops three of them, and the top of the bar lags the bend by that.
STILL_WEIGHT_SLACK = 0.05
TOLERANCE = 1e-4


class FbxToGlbTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = fbx_to_glb.convert(PROBE)
        cls.document, cls.binary = gltf_read.parse_glb(cls.data)
        cls.asset = gltf_skin.SkinnedAsset(cls.document, cls.binary)

    def test_scene_is_metres_with_y_up(self):
        low = [min(p[axis] for p in self.asset.positions) for axis in range(3)]
        high = [max(p[axis] for p in self.asset.positions) for axis in range(3)]
        self.assertAlmostEqual(low[1], 0.0, delta=TOLERANCE)
        self.assertAlmostEqual(high[1], BAR_HEIGHT, delta=TOLERANCE)
        for axis in (0, 2):
            self.assertAlmostEqual(low[axis], -BAR_HALF_WIDTH, delta=TOLERANCE)
            self.assertAlmostEqual(high[axis], BAR_HALF_WIDTH, delta=TOLERANCE)

    def test_every_vertex_keeps_at_most_four_normalised_influences(self):
        for joints, weights in zip(self.asset.joint_indices, self.asset.weights):
            self.assertEqual(len(weights), MAX_INFLUENCES)
            self.assertAlmostEqual(sum(weights), 1.0, delta=TOLERANCE)
            self.assertEqual(sorted(weights, reverse=True), list(weights))
            self.assertLessEqual(max(joints), len(self.asset.joints) - 1)

    def test_a_vertex_with_more_influences_keeps_its_four_strongest(self):
        elbow = [w for p, w in zip(self.asset.positions, self.asset.weights) if abs(p[1] - ELBOW) < TOLERANCE]
        self.assertTrue(elbow)
        for weights in elbow:
            self.assertEqual(sum(1 for w in weights if w > 0), MAX_INFLUENCES)
            self.assertGreater(weights[0] + weights[1], 0.9)  # the two bones that carry the bar

    def test_the_rest_pose_leaves_the_bind_pose_alone(self):
        posed, _ = self.asset.skin(self.asset.sample(None, 0.0))
        for rest, moved in zip(self.asset.positions, posed):
            for a, b in zip(rest, moved):
                self.assertAlmostEqual(a, b, delta=TOLERANCE)

    def test_the_animation_bends_the_bar_about_x(self):
        self.assertAlmostEqual(self.asset.duration(BEND_ANIMATION), BEND_DURATION, delta=TOLERANCE)
        posed, _ = self.asset.skin(self.asset.sample(BEND_ANIMATION, BEND_DURATION))
        height = max(p[1] for p in self.asset.positions)
        cap = [q for p, q in zip(self.asset.positions, posed) if p[1] > height - TOLERANCE]
        x, y, z = (sum(q[axis] for q in cap) / len(cap) for axis in range(3))
        # The upper half turns about the elbow, from +Y to +Z (Blender's bend
        # toward -Y is glTF's +Z).
        self.assertAlmostEqual(y, ELBOW, delta=STILL_WEIGHT_SLACK)
        self.assertAlmostEqual(z, BAR_HEIGHT - ELBOW, delta=STILL_WEIGHT_SLACK)
        self.assertAlmostEqual(x, 0.0, delta=TOLERANCE)

    def test_static_import_reads_the_bind_pose(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "probe.glb"
            path.write_bytes(self.data)
            mesh = gltf_mesh.load_gltf_mesh(path)
        self.assertEqual(len(mesh.tri_v), BAR_TRIANGLES)
        self.assertAlmostEqual(max(p[1] for p in mesh.positions), BAR_HEIGHT, delta=TOLERANCE)

    def test_a_file_that_is_not_an_fbx_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "broken.fbx"
            path.write_bytes(b"this is not an FBX file")
            with self.assertRaises(ValueError):
                fbx_to_glb.convert(path)
        with self.assertRaises(ValueError):
            fbx_to_glb.convert(PROBE.with_name("missing.fbx"))


if __name__ == "__main__":
    unittest.main()
