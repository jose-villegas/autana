"""Checks the r3d bake modules on small hand-built meshes: splits stay
conforming, merges never turn a triangle over, the cluster tree keeps its
order, and a written mesh passes its own validation. The levels of detail
have their own tests in test_r3d_lod.py. Skipped where the
pinned environment (tools/r3d/requirements.txt) is not installed."""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.geometry import triangle_areas, weld
    from r3d.light import merge_matching_colours
    from r3d.lit_mesh import validate, write_lit_mesh
    from r3d.octree import build_octree, flatten_octree
    from r3d.simplify import _label_after
    from r3d.tessellate import split_marked_edges
except ImportError:
    np = None


def z_normals(p, tris):
    a, b, c = p[tris[:, 0]], p[tris[:, 1]], p[tris[:, 2]]
    return np.cross(b - a, c - a)[:, 2]


def directed_edges(tris):
    return [(int(t[i]), int(t[(i + 1) % 3])) for t in tris for i in range(3)]


def grid(n):
    """An n by n square of unit cells in the plane z = 0, two triangles each,
    counter-clockwise seen from +z."""
    p = np.array([(x, y, 0.0) for y in range(n + 1) for x in range(n + 1)])
    tris = []
    for y in range(n):
        for x in range(n):
            v = y * (n + 1) + x
            tris += [(v, v + 1, v + n + 2), (v, v + n + 2, v + n + 1)]
    return p, np.array(tris, dtype=np.int64)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SplitTests(unittest.TestCase):
    def test_a_split_keeps_the_mesh_conforming_facing_and_the_same_area(self):
        p, tris = grid(2)
        marked = {(0, 4), (4, 8), (1, 4)}  # the diagonal through the middle, and one edge off it
        out_p, out_t, _, _ = split_marked_edges(p, tris, marked)
        self.assertTrue(np.all(z_normals(out_p, out_t) > 0), "a piece was turned over")
        self.assertAlmostEqual(triangle_areas(p, tris).sum(), triangle_areas(out_p, out_t).sum())
        edges = directed_edges(out_t)
        self.assertEqual(len(edges), len(set(edges)), "two pieces share an edge in the same direction")
        for a, b in edges:
            boundary = {out_p[a][0], out_p[b][0]} <= {0.0} or {out_p[a][0], out_p[b][0]} <= {2.0} \
                or {out_p[a][1], out_p[b][1]} <= {0.0} or {out_p[a][1], out_p[b][1]} <= {2.0}
            if not boundary:
                self.assertIn((b, a), edges, "an inner edge has no neighbour across it: a T-junction")


@unittest.skipIf(np is None, "the r3d environment is not installed")
class MergeTests(unittest.TestCase):
    def test_welding_and_colour_merges_never_turn_a_triangle_over(self):
        p, tris = grid(3)
        doubled = p[tris].reshape(-1, 3)
        split = np.arange(len(doubled)).reshape(-1, 3)
        wp, wt = weld(doubled, split)
        self.assertTrue(np.all(z_normals(wp, wt) > 0))
        rgb = np.full((len(doubled), 3), 100)
        mp, _, mt = merge_matching_colours(doubled, rgb, split)
        self.assertEqual(len(mt), len(tris))
        self.assertTrue(np.all(z_normals(mp, mt) > 0))

    def test_a_simplified_triangle_takes_the_label_most_of_its_corners_had(self):
        tris_in = np.array([[0, 1, 2], [3, 4, 5]])
        labels_in = np.array([7, 9])
        kept = np.array([0, 1, 3, 4])
        tris_out = np.array([[0, 1, 2], [2, 3, 0], [0, 2, 1], [2, 0, 3]])
        self.assertEqual(_label_after(tris_in, labels_in, kept, tris_out).tolist(), [7, 9, 7, 9])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ClusterTreeTests(unittest.TestCase):
    def setUp(self):
        p, tris = grid(8)
        self.tris = tris
        self.centres = (p[tris].mean(axis=1)) * 10.0
        self.weights = np.arange(len(tris)) % 7 + 1

    def test_children_sit_together_and_a_subtree_owns_a_run_of_items(self):
        root = build_octree(self.centres, self.weights, 12, 6)
        order, nodes = flatten_octree(root)

        def span(i):
            node = nodes[i]
            if node["leaf"]:
                return node["first"], node["first"] + node["count"]
            parts = [span(c) for c in range(node["first"], node["first"] + node["count"])]
            for (_, end), (start, _) in zip(parts, parts[1:]):
                self.assertEqual(end, start, "a subtree's items are not one run")
            for c in range(node["first"], node["first"] + node["count"]):
                self.assertGreater(c, i, "a child comes before its parent")
            return parts[0][0], parts[-1][1]

        self.assertEqual(span(0), (0, len(order)))
        self.assertEqual(sorted(order), list(range(len(self.tris))))

    def test_a_leaf_holds_one_item_or_no_more_than_its_weight(self):
        root = build_octree(self.centres, self.weights, 12, 6)
        order, nodes = flatten_octree(root)
        for node in nodes:
            if node["leaf"]:
                held = order[node["first"] : node["first"] + node["count"]]
                self.assertTrue(len(held) == 1 or self.weights[held].sum() <= 12)

    def test_a_depth_of_zero_keeps_everything_in_one_leaf(self):
        order, nodes = flatten_octree(build_octree(self.centres, self.weights, 1, 0))
        self.assertEqual(len(nodes), 1)
        self.assertEqual(nodes[0]["count"], len(order))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class LitMeshTests(unittest.TestCase):
    def test_a_written_mesh_names_its_counts(self):
        p, tris = grid(6)
        rgb = np.full((len(p), 3), 128)
        with tempfile.TemporaryDirectory() as out:
            mesh = write_lit_mesh(out, "demo", p * 50.0, rgb, tris, np.zeros(len(tris), dtype=np.int64), ["test"], 16, 4)
            header = (pathlib.Path(out) / "demo_mesh_generated.h").read_text()
        self.assertIn(f"#define DEMO_TRIANGLE_COUNT {len(tris)}", header)
        self.assertEqual(len(mesh.tris), len(tris))

    def test_validation_refuses_a_triangle_reaching_outside_its_cluster(self):
        pos = np.zeros((6, 3), dtype=np.int64)
        tris = np.array([[0, 1, 2], [3, 4, 0]])
        clusters = [(0, 3, 0, 1, np.zeros(3), np.zeros(3), False), (3, 3, 1, 1, np.zeros(3), np.zeros(3), False)]
        nodes = [{"leaf": True, "first": 0, "count": 2}]
        with self.assertRaises(AssertionError):
            validate(pos, np.zeros((6, 3)), tris, clusters, nodes)

    def test_validation_refuses_more_triangles_than_uint16_offsets_hold(self):
        pos = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]])
        tris = np.tile([0, 1, 2], (65536, 1))
        with self.assertRaisesRegex(AssertionError, "triangles"):
            validate(pos, np.zeros((3, 3)), tris, [], [])

    def test_validation_refuses_more_clusters_than_uint16_offsets_hold(self):
        pos = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]])
        clusters = [(0, 3, 0, 1, np.zeros(3), np.zeros(3), False)] * 65536
        with self.assertRaisesRegex(AssertionError, "clusters"):
            validate(pos, np.zeros((3, 3)), np.array([[0, 1, 2]]), clusters, [])


if __name__ == "__main__":
    unittest.main()
