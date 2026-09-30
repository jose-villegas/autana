"""Checks the r3d bake modules on small hand-built meshes: splits stay
conforming, merges never turn a triangle over, the cluster tree keeps its
order, meshlets keep their size, hold every triangle once and bake to the
same bytes however the triangles arrive, and a written mesh passes its own
validation. Skipped where the
pinned environment (tools/r3d/requirements.txt) is not installed."""

import collections
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.geometry import triangle_areas, weld
    from r3d.light import merge_matching_colours
    from r3d import lit_mesh, rebake
    from r3d.lit_mesh import MESHLET_TRIANGLES, bake_lit_mesh, read_lit_mesh, validate, weld_quantised, write_lit_mesh
    from r3d.meshopt import build_meshlets
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
            mesh = write_lit_mesh(out, "demo", p * 50.0, rgb, tris, np.zeros(len(tris), dtype=np.int64), ["test"],
                                  leaf_triangles=16, max_depth=4)
            header = (pathlib.Path(out) / "demo_mesh_generated.h").read_text()
        self.assertIn(f"#define DEMO_TRIANGLE_COUNT {len(tris)}", header)
        self.assertEqual(len(mesh.tris), len(tris))

    def test_validation_refuses_a_triangle_reaching_outside_its_cluster(self):
        pos = np.zeros((6, 3), dtype=np.int64)
        tris = np.array([[0, 1, 2], [3, 4, 0]])
        clusters = [(0, 3, 0, 1, np.zeros(3), np.zeros(3), False), (3, 3, 1, 1, np.zeros(3), np.zeros(3), False)]
        nodes = [{"leaf": True, "first": 0, "count": 2}]
        with self.assertRaises(AssertionError):
            validate(pos, np.zeros((6, 3)), tris, clusters, nodes, [])

    def test_validation_refuses_more_triangles_than_uint16_offsets_hold(self):
        pos = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]])
        tris = np.tile([0, 1, 2], (65536, 1))
        with self.assertRaisesRegex(AssertionError, "triangles"):
            validate(pos, np.zeros((3, 3)), tris, [], [], [])

    def test_validation_refuses_more_clusters_than_uint16_offsets_hold(self):
        pos = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]])
        clusters = [(0, 3, 0, 1, np.zeros(3), np.zeros(3), False)] * 65536
        with self.assertRaisesRegex(AssertionError, "clusters"):
            validate(pos, np.zeros((3, 3)), np.array([[0, 1, 2]]), clusters, [], [])


def sphere(subdivisions, radius=400.0):
    """A subdivided icosahedron, outward counter-clockwise, coloured by height."""
    t = (1 + 5**0.5) / 2
    v = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
         (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    f = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6),
         (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10),
         (8, 6, 7), (9, 8, 1)]
    verts = [np.array(x, dtype=float) / np.linalg.norm(x) for x in v]
    for _ in range(subdivisions):
        cache, out = {}, []

        def mid(a, b):
            key = (min(a, b), max(a, b))
            if key not in cache:
                m = verts[a] + verts[b]
                verts.append(m / np.linalg.norm(m))
                cache[key] = len(verts) - 1
            return cache[key]

        for a, b, c in f:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            out += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        f = out
    p = np.array(verts) * radius
    rgb = np.column_stack([128 + 100 * np.sin(p[:, 2] / 60), 128 + 100 * np.cos(p[:, 0] / 90), np.full(len(p), 90.0)])
    return p, rgb, np.array(f), np.zeros(len(f), dtype=np.int64)


def sheet(n, size=800.0):
    """An n by n bumpy sheet, its half at x > 0 double sided."""
    xs = np.linspace(-size / 2, size / 2, n + 1)
    p = np.array([(x, y, 30 * np.sin(x / 50) * np.cos(y / 70)) for y in xs for x in xs])
    rgb = np.column_stack([128 + 90 * np.sin(p[:, 0] / 40), 128 + 90 * np.sin(p[:, 1] / 55), np.full(len(p), 60.0)])
    tris = []
    for y in range(n):
        for x in range(n):
            a = y * (n + 1) + x
            tris += [(a, a + 1, a + n + 2), (a, a + n + 2, a + n + 1)]
    tris = np.array(tris)
    return p, rgb, tris, (p[tris].mean(axis=1)[:, 0] > 0).astype(np.int64)


def canonical(pos, tris):
    """Each triangle as its three positions turned to start at the smallest."""
    out = []
    for t in tris:
        corners = [tuple(int(x) for x in pos[i]) for i in t]
        k = corners.index(min(corners))
        out.append(tuple(corners[k:] + corners[:k]))
    return collections.Counter(out)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class MeshletTests(unittest.TestCase):
    """Every check runs on a closed sphere and an open two-sided sheet."""

    @classmethod
    def setUpClass(cls):
        cls.inputs = {"sphere": sphere(3), "sheet": sheet(28)}
        cls.baked = {name: bake_lit_mesh(*m) for name, m in cls.inputs.items()}

    def each(self):
        for name in self.inputs:
            with self.subTest(mesh=name):
                yield self.inputs[name], self.baked[name]

    def test_a_meshlet_stays_within_the_default_size_and_most_are_nearly_full(self):
        for _, mesh in list(self.each()):
            sizes = np.array([c[3] for c in mesh.clusters])
            self.assertLessEqual(sizes.max(), MESHLET_TRIANGLES)
            self.assertLessEqual(max(c[1] for c in mesh.clusters), MESHLET_TRIANGLES)
            self.assertGreaterEqual(np.percentile(sizes, 50), MESHLET_TRIANGLES // 2)

    def test_a_size_asked_for_is_the_size_kept(self):
        for name, m in self.inputs.items():
            mesh = bake_lit_mesh(*m, meshlet_triangles=20)
            self.assertLessEqual(max(c[3] for c in mesh.clusters), 20)
            self.assertGreater(len(mesh.clusters), len(self.baked[name].clusters))

    def test_the_binding_cuts_every_triangle_into_exactly_one_meshlet(self):
        p, _, tris, _ = self.inputs["sphere"]
        meshlets = build_meshlets(p, tris, 24)
        self.assertEqual(sum(len(m) for m in meshlets), len(tris))
        self.assertLessEqual(max(len(m) for m in meshlets), 24)
        key = lambda t: tuple(sorted(t))  # noqa: E731
        self.assertEqual(collections.Counter(map(key, np.concatenate(meshlets).tolist())),
                         collections.Counter(map(key, tris.tolist())))

    def test_every_triangle_is_in_one_cluster_and_no_other(self):
        for m, mesh in list(self.each()):
            p, rgb, tris, double = m
            q = np.round(p * mesh.position_scale).astype(np.int64)
            wq, _, wt, _ = weld_quantised(q, np.clip(np.rint(rgb), 0, 255).astype(np.int64), tris, double)
            self.assertEqual(canonical(mesh.pos, mesh.tris), canonical(wq, wt))

    def test_a_cluster_is_of_one_sidedness_and_both_kinds_are_there(self):
        for m, mesh in list(self.each()):
            self.assertEqual({c[6] for c in mesh.clusters}, {bool(d) for d in np.unique(m[3])})

    def test_the_tree_is_built_over_the_meshlets(self):
        for _, mesh in list(self.each()):
            self.assertEqual(sum(n["count"] for n in mesh.nodes if n["leaf"]), len(mesh.clusters))
            self.assertLess(len(mesh.nodes), len(mesh.clusters))

    @staticmethod
    def culled(cone, eye):
        """r3d_lit_pipeline.c's test, on the same numbers."""
        apex, axis, cutoff = np.array(cone[0], dtype=np.float32), np.array(cone[1], dtype=np.float32), cone[2]
        d = apex - eye.astype(np.float32)
        a = float(d @ axis)
        return cutoff < lit_mesh.CONE_NEVER and a > 0 and a * a >= cutoff * cutoff * float(d @ d)

    def test_a_cluster_a_cone_culls_has_no_triangle_facing_the_eye(self):
        rng = np.random.default_rng(5)
        for _, mesh in list(self.each()):
            eyes = rng.uniform(-900, 900, (300, 3)) * mesh.position_scale
            dropped = 0
            for cluster, cone in zip(mesh.clusters, mesh.cones):
                v = mesh.pos[cluster[0] : cluster[0] + cluster[1]].astype(np.float64)
                t = mesh.tris[cluster[2] : cluster[2] + cluster[3]] - cluster[0]
                n = np.cross(v[t[:, 1]] - v[t[:, 0]], v[t[:, 2]] - v[t[:, 0]])
                for eye in eyes:
                    if self.culled(cone, eye):
                        dropped += 1
                        self.assertFalse(cluster[6])
                        self.assertTrue(np.all(np.einsum("ij,ij->i", n, eye - v[t[:, 0]]) <= 0))
            self.assertGreater(dropped, 0)

    def test_a_double_sided_cluster_gets_a_cone_that_never_culls(self):
        for _, mesh in list(self.each()):
            self.assertEqual(len(mesh.cones), len(mesh.clusters))
            for cluster, cone in zip(mesh.clusters, mesh.cones):
                if cluster[6]:
                    self.assertEqual(cone[2], lit_mesh.CONE_NEVER)

    def test_the_c_data_reads_back_as_what_was_baked(self):
        for m, mesh in list(self.each()):
            with tempfile.TemporaryDirectory() as out:
                write_lit_mesh(out, "demo", *m, ["test"])
                back = read_lit_mesh(pathlib.Path(out) / "demo_mesh_generated.c")
            self.assertTrue(np.array_equal(back.pos, mesh.pos) and np.array_equal(back.tris, mesh.tris))
            self.assertEqual([(n["first"], n["count"], n["leaf"]) for n in back.nodes],
                             [(n["first"], n["count"], n["leaf"]) for n in mesh.nodes])

    def test_the_same_triangles_bake_to_the_same_mesh_however_they_arrive(self):
        for m, mesh in list(self.each()):
            p, rgb, tris, double = m
            order = np.random.default_rng(3).permutation(len(tris))
            turned = np.array([np.roll(t, i % 3) for i, t in enumerate(tris[order])])
            again = bake_lit_mesh(p, rgb, turned, double[order])
            self.assertTrue(np.array_equal(again.pos, mesh.pos) and np.array_equal(again.tris, mesh.tris))

    def test_rebaking_a_mesh_is_a_fixed_point_and_keeps_its_triangles(self):
        for m, mesh in list(self.each()):
            with tempfile.TemporaryDirectory() as out:
                banner = ["GENERATED FILE - do not edit.", "", "    python gen.py --out-dir .", "", "Some Model (CC BY)"]
                write_lit_mesh(out, "demo", *m, banner)
                c = pathlib.Path(out) / "demo_mesh_generated.c"
                rebake.main([str(c), "--meshlet-triangles", "24"])
                once = (c.read_text(), c.with_suffix(".h").read_text())
                rebake.main([str(c), "--meshlet-triangles", "24"])
                twice = (c.read_text(), c.with_suffix(".h").read_text())
                back = read_lit_mesh(c)
            self.assertEqual(once, twice)
            self.assertEqual(canonical(back.pos, back.tris), canonical(mesh.pos, mesh.tris))
            head = once[0][: once[0].index("*/")]
            self.assertEqual(head.count("python "), 2, "the banner names the rebake command and the original")
            self.assertIn("python launcher/tools/r3d/rebake.py ", head)
            self.assertIn("Some Model (CC BY)", head)
            self.assertIn("--meshlet-triangles 24", head)

if __name__ == "__main__":
    unittest.main()
