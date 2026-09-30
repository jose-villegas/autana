"""The committed capybara.glb against the glTF rules a skinned-mesh baker
relies on, and against the generator that made it.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests

Everything is read back from the file's own bytes: the mesh must be closed
and wound outwards, every vertex weighted to at most four nearby joints, the
inverse bind matrices the bind pose's inverses, both clips closed loops, and
the walk's planted feet must neither sink nor skid.
"""
import math
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
ASSET = TOOLS.parent / "assets" / "capybara.glb"
GENERATOR = TOOLS / "gen_capybara.py"
sys.path.insert(0, str(TOOLS.parents[3] / "tools"))
from gltf import gltf_read  # noqa: E402
from r3d import gltf_skin  # noqa: E402

FPS = 30
MAX_INFLUENCE_DISTANCE = 0.40
TOLERANCE = 1e-5


def distance_to_segment(p, a, b):
    ab = [y - x for x, y in zip(a, b)]
    ap = [y - x for x, y in zip(a, p)]
    length = sum(v * v for v in ab)
    t = 0.0 if length == 0.0 else min(max(sum(x * y for x, y in zip(ap, ab)) / length, 0.0), 1.0)
    return math.sqrt(sum((x - (a0 + t * d)) ** 2 for x, a0, d in zip(p, a, ab)))


def translation_of(matrix):
    return (matrix[3], matrix[7], matrix[11])


class CapybaraAssetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = ASSET.read_bytes()
        cls.document, cls.binary = gltf_read.parse_glb(cls.data)
        cls.asset = gltf_skin.SkinnedAsset(cls.document, cls.binary)
        cls.rest_world = cls.asset.world_matrices(cls.asset.sample(None, 0.0))

    def test_accessors_lie_inside_their_buffer_views(self):
        self.assertEqual(self.document["buffers"][0]["byteLength"], len(self.binary))
        for view in self.document["bufferViews"]:
            self.assertLessEqual(view["byteOffset"] + view["byteLength"], len(self.binary))
        for accessor in self.document["accessors"]:
            view = self.document["bufferViews"][accessor["bufferView"]]
            _, size = gltf_read.COMPONENT_FORMATS[accessor["componentType"]]
            element = size * gltf_read.TYPE_WIDTHS[accessor["type"]]
            stride = view.get("byteStride", element)
            end = accessor.get("byteOffset", 0) + stride * (accessor["count"] - 1) + element
            self.assertLessEqual(end, view["byteLength"])
            self.assertEqual((view["byteOffset"] + accessor.get("byteOffset", 0)) % size, 0)

    def test_mesh_is_small_closed_and_wound_outwards(self):
        triangles = self.asset.triangles
        self.assertTrue(800 <= len(triangles) <= 1500, len(triangles))
        vertex_count = len(self.asset.positions)
        edges = {}
        for tri in triangles:
            self.assertTrue(all(0 <= i < vertex_count for i in tri))
            for edge in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
                edges[edge] = edges.get(edge, 0) + 1
        for (a, b), count in edges.items():
            self.assertEqual((count, edges.get((b, a))), (1, 1), "edge %d-%d" % (a, b))
        for shell in self.shells():
            volume = 0.0
            for a, b, c in shell:
                pa, pb, pc = (self.asset.positions[i] for i in (a, b, c))
                volume += (
                    pa[0] * (pb[1] * pc[2] - pb[2] * pc[1])
                    - pa[1] * (pb[0] * pc[2] - pb[2] * pc[0])
                    + pa[2] * (pb[0] * pc[1] - pb[1] * pc[0])
                )
            self.assertGreater(volume, 0.0)

    def shells(self):
        parent = list(range(len(self.asset.positions)))

        def find(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i

        for a, b, c in self.asset.triangles:
            parent[find(b)] = find(a)
            parent[find(c)] = find(a)
        groups = {}
        for tri in self.asset.triangles:
            groups.setdefault(find(tri[0]), []).append(tri)
        return list(groups.values())

    def test_normals_are_unit_length(self):
        for n in self.asset.normals:
            self.assertAlmostEqual(math.sqrt(sum(c * c for c in n)), 1.0, delta=1e-4)

    def test_weights_are_four_normalised_influences_on_valid_joints(self):
        joint_count = len(self.asset.joints)
        self.assertLessEqual(joint_count, 20)
        for joints, weights in zip(self.asset.joint_indices, self.asset.weights):
            self.assertEqual(len(joints), 4)
            self.assertAlmostEqual(sum(weights), 1.0, delta=TOLERANCE)
            used = [j for j, w in zip(joints, weights) if w > 0.0]
            self.assertTrue(all(0 <= j < joint_count for j in joints))
            self.assertEqual(len(used), len(set(used)))
            self.assertTrue(all(w >= 0.0 for w in weights))

    def test_every_influence_is_near_its_bone(self):
        rest = [translation_of(self.rest_world[node]) for node in self.asset.joints]
        children = {i: [] for i in range(len(self.asset.joints))}
        slot = {node: i for i, node in enumerate(self.asset.joints)}
        for i, node in enumerate(self.asset.joints):
            parent = self.asset.parents[node]
            if parent in slot:
                children[slot[parent]].append(i)
        for p, joints, weights in zip(self.asset.positions, self.asset.joint_indices, self.asset.weights):
            for j, w in zip(joints, weights):
                if w > 0.0:
                    ends = children[j] or [j]
                    distance = min(distance_to_segment(p, rest[j], rest[e]) for e in ends)
                    self.assertLessEqual(distance, MAX_INFLUENCE_DISTANCE, (p, j))

    def test_inverse_bind_matrices_invert_the_bind_pose(self):
        for node, inverse in zip(self.asset.joints, self.asset.inverse_binds):
            product = gltf_skin.mat_mul(self.rest_world[node], inverse)
            for index, value in enumerate(product):
                self.assertAlmostEqual(value, 1.0 if index % 5 == 0 else 0.0, delta=TOLERANCE)

    def test_idle_and_walk_are_closed_loops_at_a_fixed_rate(self):
        durations = {name: self.asset.duration(name) for name in self.asset.animations}
        self.assertEqual(sorted(durations), ["idle", "walk"])
        self.assertTrue(3.0 <= durations["idle"] <= 4.0)
        self.assertTrue(0.8 <= durations["walk"] <= 1.2)
        for name, animation in self.asset.animations.items():
            for sampler in animation["samplers"]:
                self.assertEqual(sampler["interpolation"], "LINEAR")
                times = [t[0] for t in gltf_read.read_accessor(self.document, self.binary, sampler["input"])]
                self.assertEqual(times[0], 0.0)
                for a, b in zip(times, times[1:]):
                    self.assertAlmostEqual(b - a, 1.0 / FPS, delta=1e-4)
                values = gltf_read.read_accessor(self.document, self.binary, sampler["output"])
                self.assertEqual(values[0], values[-1], name)
            for channel in animation["channels"]:
                self.assertIn(channel["target"]["path"], ("translation", "rotation"))

    def foot_nodes(self):
        """Joints that carry the lowest vertices: the feet, found by weight."""
        lowest = {}
        for p, joints, weights in zip(self.asset.positions, self.asset.joint_indices, self.asset.weights):
            if p[1] < 0.005:
                joint = joints[max(range(4), key=lambda k: weights[k])]
                lowest[joint] = True
        return [self.asset.joints[j] for j in sorted(lowest)]

    def test_idle_keeps_the_feet_planted(self):
        feet = self.foot_nodes()
        self.assertEqual(len(feet), 4)
        rest = {n: translation_of(self.rest_world[n]) for n in feet}
        duration = self.asset.duration("idle")
        for frame in range(0, int(duration * FPS), 5):
            world = self.asset.world_matrices(self.asset.sample("idle", frame / FPS))
            for n in feet:
                moved = math.dist(translation_of(world[n]), rest[n])
                self.assertLess(moved, 0.002, (frame, n))

    def test_walk_feet_neither_sink_nor_skid(self):
        feet = self.foot_nodes()
        duration = self.asset.duration("walk")
        frames = int(round(duration * FPS))
        tracks = {n: [] for n in feet}
        for frame in range(frames):
            pose = self.asset.sample("walk", frame / FPS)
            positions, _ = self.asset.skin(pose)
            self.assertGreater(min(p[1] for p in positions), -0.004, frame)
            world = self.asset.world_matrices(pose)
            for n in feet:
                tracks[n].append(translation_of(world[n]))
        belt = []
        for n, track in tracks.items():
            ground = min(p[1] for p in track)
            planted = [i for i, p in enumerate(track) if p[1] < ground + 0.001]
            self.assertGreater(len(planted), frames // 2, n)
            for i in planted:
                if i + 1 in planted:
                    belt.append(track[i + 1][2] - track[i][2])
                    self.assertAlmostEqual(track[i + 1][0], track[i][0], delta=0.001)
        # In place, a planted foot rides a treadmill: one constant backward speed.
        self.assertLess(max(belt), 0.0)
        self.assertLess(max(belt) - min(belt), 0.1 * abs(sum(belt) / len(belt)))

    def test_generator_is_deterministic_and_the_asset_current(self):
        outputs = []
        with tempfile.TemporaryDirectory() as scratch:
            for run in range(2):
                out = pathlib.Path(scratch) / ("run%d.glb" % run)
                subprocess.run(
                    [sys.executable, str(GENERATOR), "--out", str(out)],
                    check=True, capture_output=True,
                )
                outputs.append(out.read_bytes())
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(outputs[0], self.data, "capybara.glb is stale: regenerate it")


if __name__ == "__main__":
    unittest.main()
