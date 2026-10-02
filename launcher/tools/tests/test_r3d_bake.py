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
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    import trimesh
    from trimesh.ray.ray_pyembree import RayMeshIntersector

    from r3d.geometry import triangle_areas, weld
    from r3d.light import IndirectCache, adaptive_sample_counts, build_indirect_cache, face_colours, gather_indirect, light, merge_matching_colours
    from r3d import lit_mesh, rebake
    from r3d.lit_mesh import MESHLET_TRIANGLES, bake_lit_mesh, read_lit_mesh, validate, weld_quantised, write_lit_mesh
    from r3d.meshopt import build_meshlets, simplify_with_update
    from r3d.octree import build_octree, flatten_octree
    from r3d.repair import _weld_borders, repair
    from r3d.simplify import SEAM_COLOUR_TOLERANCE, _label_after, merge_close_colours, simplify
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


def open_edges(p, tris):
    """The edges a single triangle uses, as pairs of vertex positions."""
    key = np.round(p / 1e-6).astype(np.int64)
    _, ids = np.unique(key, axis=0, return_inverse=True)
    ids = ids.reshape(-1)[tris]
    edges = np.sort(np.concatenate([ids[:, [0, 1]], ids[:, [1, 2]], ids[:, [2, 0]]]), axis=1)
    unique, count = np.unique(edges, axis=0, return_counts=True)
    first = np.zeros(ids.max() + 1, dtype=np.int64)
    first[ids.reshape(-1)] = tris.reshape(-1)
    return [(tuple(p[first[a]]), tuple(p[first[b]])) for a, b in unique[count == 1]]


def on_seam(edge):
    return all(abs(v[0]) < 1e-6 for v in edge)


def two_pieces(gap=0.0):
    """A coarse piece left of the line x = 0 and a finer one right of it, `gap`
    apart: the finer piece has a vertex at every unit of y along the line,
    the coarse piece only at its ends, so those vertices sit on the coarse
    piece's edge."""
    p = [(-4, 0), (0, 0), (0, 4), (-4, 4)] + [(gap, y) for y in range(5)] + [(4 + gap, 0), (4 + gap, 4)]
    far0, far4 = 9, 10
    tris = [(0, 1, 2), (0, 2, 3)] + [(4 + k, far0, 5 + k) for k in range(4)] + [(8, far0, far4)]
    return np.array([(x, y, 0.0) for x, y in p]), np.array(tris)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class RepairTests(unittest.TestCase):
    def repaired(self, p, tris, tolerance=0.01):
        rgb = np.full((len(p), 3), 100.0)
        return repair(p, rgb, tris, np.arange(len(tris)) % 2, tolerance)

    def test_touching_pieces_share_their_seam_through_simplification(self):
        p, tris = two_pieces()
        self.assertTrue(any(on_seam(e) for e in open_edges(p, tris)), "the fixture has no open seam")
        rp, rgb, rt, _ = self.repaired(p, tris)
        self.assertFalse(any(on_seam(e) for e in open_edges(rp, rt)), "the seam is still open")
        sp, _, st, _ = simplify_with_update(rp, rgb, rt, 4)
        self.assertLess(len(st), len(rt))
        self.assertFalse(any(on_seam(e) for e in open_edges(sp, st)), "simplification opened the seam")

    def test_repair_keeps_the_area_the_facing_and_every_original_vertex(self):
        p, tris = two_pieces()
        rgb = np.array([(10.0 * i, 0.0, 0.0) for i in range(len(p))])
        rp, rc, rt, _ = repair(p, rgb, tris, np.zeros(len(tris), dtype=int), 0.01)
        self.assertAlmostEqual(triangle_areas(p, tris).sum(), triangle_areas(rp, rt).sum())
        self.assertTrue(np.all(z_normals(rp, rt) > 0), "a piece was turned over")
        self.assertEqual(len(rp), len(rc))
        self.assertTrue(np.array_equal(rc[: len(p)], rgb), "an original vertex changed colour")
        self.assertTrue(np.array_equal(rp[: len(p)], p), "an original vertex moved")

    def test_a_gap_wider_than_the_tolerance_is_not_closed(self):
        p, tris = two_pieces(gap=0.5)
        rp, _, rt, _ = self.repaired(p, tris, tolerance=0.1)
        self.assertTrue(np.array_equal(rp, p))
        self.assertTrue(np.array_equal(rt, tris))
        self.assertGreater(len(open_edges(rp, rt)), 0)

    def test_a_connected_mesh_is_returned_as_it_came(self):
        p, tris = grid(4)
        rp, rgb, rt, labels = self.repaired(p, tris)
        self.assertTrue(np.array_equal(rp, p))
        self.assertTrue(np.array_equal(rt, tris))
        self.assertEqual(len(rgb), len(p))
        self.assertEqual(len(labels), len(tris))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class RepairStepTests(unittest.TestCase):
    def test_border_vertices_within_the_tolerance_end_at_one_position_and_no_further(self):
        tolerance = 0.125  # a power of two, so a distance can sit exactly on it
        for offset, welded in ((0.0625, True), (tolerance, True), (0.126, False)):
            p = np.array([(0, 0, 0), (1, 0, 0), (0, 1, 0), (1 + offset, 0, 0), (2, 0, 0), (1 + offset, 1, 0)], dtype=float)
            out = _weld_borders(p, np.array([(0, 1, 2), (3, 4, 5)]), tolerance)
            self.assertEqual(bool(np.array_equal(out[1], out[3])), welded, f"offset {offset}")

    def test_a_weld_closes_the_seam_between_pieces_offset_by_less_than_the_tolerance(self):
        p, tris = two_pieces(gap=0.0625)
        rp, _, rt, _ = repair(p, np.zeros((len(p), 3)), tris, np.zeros(len(tris), dtype=int), 0.125)
        self.assertFalse(any(0 < (a[1] + b[1]) / 2 < 4 and abs(a[0] + b[0]) < 0.3 for a, b in open_edges(rp, rt)))

    def test_interior_vertices_within_the_tolerance_are_never_moved(self):
        p, tris = grid(4)
        p = p * 0.1
        edges = directed_edges(tris)
        border = {a for a, b in edges if (b, a) not in set(edges)} | {b for a, b in edges if (b, a) not in set(edges)}
        interior = [i for i in range(len(p)) if i not in border]
        self.assertTrue(interior)
        out = _weld_borders(p, tris, 0.2)
        self.assertTrue(np.array_equal(out[interior], p[interior]))

    def test_a_vertex_added_on_a_split_edge_takes_the_colour_the_edge_has_there(self):
        p, tris = two_pieces()
        rgb = np.zeros((len(p), 3))
        rgb[2] = (80.0, 0.0, 0.0)  # the coarse piece's seam runs from vertex 1 to vertex 2, y = 0 to 4
        _, out, _, _ = repair(p, rgb, tris, np.zeros(len(tris), dtype=int), 0.01)
        self.assertEqual(sorted(out[len(p):, 0].tolist()), [20.0, 40.0, 60.0])

    def test_repairing_repaired_output_adds_and_moves_nothing(self):
        p, tris = two_pieces(gap=0.0625)
        once = repair(p, np.zeros((len(p), 3)), tris, np.zeros(len(tris), dtype=int), 0.125)
        twice = repair(*once, 0.125)
        for first, second in zip(once, twice):
            self.assertTrue(np.array_equal(first, second))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ColourSeamTests(unittest.TestCase):
    TOLERANCE = np.array([12, 6, 12])

    def merged(self, colours, tolerance=None):
        colours = np.array(colours, dtype=float)
        return merge_close_colours(np.zeros((len(colours), 3), dtype=np.int64), colours, self.TOLERANCE if tolerance is None else tolerance)

    def test_a_colour_exactly_at_the_tolerance_merges_and_one_over_does_not(self):
        self.assertEqual(len({tuple(c) for c in self.merged([(100, 100, 100), (112, 106, 88)])}), 1)
        self.assertEqual(len({tuple(c) for c in self.merged([(100, 100, 100), (113, 100, 100)])}), 2)
        self.assertEqual(len({tuple(c) for c in self.merged([(100, 100, 100), (100, 107, 100)])}), 2)

    def test_colours_at_different_positions_stay_apart_however_close(self):
        q = np.arange(6).reshape(2, 3)
        rgb = np.array([(50.0, 50.0, 50.0), (55.0, 50.0, 50.0)])
        self.assertTrue(np.array_equal(merge_close_colours(q, rgb, self.TOLERANCE), rgb))

    def test_a_chain_of_colours_leaves_only_colours_further_apart_than_the_tolerance(self):
        out = self.merged([(0, 0, 0), (8, 0, 0), (16, 0, 0), (24, 0, 0)], np.array([10, 10, 10]))
        kept = sorted({tuple(c) for c in out})
        self.assertEqual(len(kept), 2)
        self.assertGreater(kept[1][0] - kept[0][0], 10)
        self.assertTrue(np.array_equal(self.merged(out, np.array([10, 10, 10])), out), "merging again changed the colours")


def seam_fixture():
    """Two triangles' worth of flat-shaded grid corners: every triangle owns
    its corners, and corners at one position differ by a few colour levels."""
    p, tris = grid(2)
    corners = p[tris].reshape(-1, 3)
    rgb = np.tile((100.0, 100.0, 100.0), (len(corners), 1))
    rgb[:, 0] += 3.0 * (np.arange(len(corners)) % 2)
    return corners, rgb, np.arange(len(corners)).reshape(-1, 3), np.zeros(len(tris), dtype=int)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SealSeamsTests(unittest.TestCase):
    def open_seam(self, seal, gap, scale):
        p, tris = two_pieces(gap)
        sp, _, st, _ = simplify(p, np.full((len(p), 3), 100.0), tris, np.arange(len(tris)) % 2, 4, seal_seams=seal,
                                position_scale=scale)
        return any(0.5 < (a[1] + b[1]) / 2 < 3.5 and abs(a[0] + b[0]) / 2 < gap + 0.3 for a, b in open_edges(sp, st))

    def test_pieces_offset_by_less_than_a_quantum_are_sealed_and_by_more_are_not(self):
        scale = 4  # a quantum of 0.25 model units
        self.assertTrue(self.open_seam(False, 0.2, scale), "the seam was closed without the option")
        self.assertFalse(self.open_seam(True, 0.2, scale), "an offset under a quantum stayed open")
        self.assertTrue(self.open_seam(True, 0.3, scale), "an offset over a quantum was closed")

    def seams(self, seal):
        corners, rgb, tris, labels = seam_fixture()
        sp, sc, st, _ = simplify(corners, rgb, tris, labels, len(tris), seal_seams=seal)
        mesh = bake_lit_mesh(sp, np.rint(sc).astype(np.int64), st, np.zeros(len(st), dtype=int))
        return len(mesh.pos) - len({tuple(v) for v in mesh.pos.tolist()})

    def test_the_default_keeps_a_seam_of_near_colours_and_the_option_merges_it(self):
        self.assertGreater(self.seams(False), 0)
        self.assertEqual(self.seams(True), 0)


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
    def test_a_flat_bake_keeps_one_centre_colour_per_triangle_and_welds_positions(self):
        p = np.array([(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 0), (1, 1, 0)])
        rgb = np.array([(1, 2, 3), (4, 5, 6), (7, 8, 9), (200, 201, 202), (10, 11, 12)])
        tris = np.array([(0, 1, 2), (3, 4, 1)])
        centres = p[tris].mean(axis=1)
        sampled = np.column_stack((centres[:, 0] * 255, centres[:, 1] * 255, np.zeros(len(centres))))
        mesh = bake_lit_mesh(p, rgb, tris, np.zeros(len(tris), dtype=int), face_rgb=sampled)
        self.assertIsNone(mesh.rgb)
        self.assertEqual(len(mesh.pos), 4)
        self.assertEqual(sorted(mesh.face_colors.tolist()), sorted(np.rint(sampled).astype(int).tolist()))

    def test_back_to_back_triangles_keep_their_own_face_colours(self):
        p = np.array([(0, 0, 0), (1, 0, 0), (0, 1, 0)])
        tris = np.array([(0, 1, 2), (0, 2, 1)])
        faces = np.array([(10, 20, 30), (200, 210, 220)])
        mesh = bake_lit_mesh(p, None, tris, np.zeros(2, dtype=int), face_rgb=faces)
        def area(t):
            a, bb, c = mesh.pos[t][:, :2]
            return np.cross(bb - a, c - a)

        by_side = {bool(area(t) > 0): tuple(c) for t, c in zip(mesh.tris, mesh.face_colors)}
        self.assertEqual(by_side[True], (10, 20, 30))
        self.assertEqual(by_side[False], (200, 210, 220))

    def test_a_smooth_bake_carries_no_face_colours(self):
        p, tris = grid(2)
        mesh = bake_lit_mesh(p, np.full((len(p), 3), 128), tris, np.zeros(len(tris), dtype=int))
        self.assertIsNone(mesh.face_colors)
        self.assertEqual(len(mesh.rgb), len(mesh.pos))

    def test_a_flat_mesh_entry_has_face_colours_and_a_smooth_one_has_vertex_colours_never_both(self):
        p, tris = grid(2)
        double = np.zeros(len(tris), dtype=int)
        with tempfile.TemporaryDirectory() as out:
            write_lit_mesh(out, "smooth", p, np.full((len(p), 3), 128), tris, double)
            write_lit_mesh(out, "solid", p, None, tris, double, face_rgb=np.full((len(tris), 3), 64))
            smooth = (pathlib.Path(out) / "smooth.mesh").read_bytes()
            solid = (pathlib.Path(out) / "solid.mesh").read_bytes()
        smooth_at = lit_mesh.BLOB_HEADER.unpack_from(smooth)[5:]
        solid_at = lit_mesh.BLOB_HEADER.unpack_from(solid)[5:]
        self.assertEqual((smooth_at[1] != 0, smooth_at[5] != 0), (True, False))
        self.assertEqual((solid_at[1] != 0, solid_at[5] != 0), (False, True))
        face = np.frombuffer(solid, dtype="<u2", count=len(tris), offset=solid_at[5])
        self.assertTrue((face == 0x0842).all(), "0x404040 in the panel's byte-swapped RGB565")

    def test_a_written_mesh_names_its_counts_and_keeps_every_array_inside_the_entry(self):
        p, tris = grid(6)
        rgb = np.full((len(p), 3), 128)
        with tempfile.TemporaryDirectory() as out:
            mesh = write_lit_mesh(out, "demo", p * 50.0, rgb, tris, np.zeros(len(tris), dtype=np.int64),
                                  leaf_triangles=16, max_depth=4)
            blob = (pathlib.Path(out) / "demo.mesh").read_bytes()
        vertices, triangles, clusters, nodes, scale, *at = lit_mesh.BLOB_HEADER.unpack_from(blob)
        self.assertEqual((vertices, triangles, clusters, nodes), (len(mesh.pos), len(tris), len(mesh.clusters), len(mesh.nodes)))
        sizes = (vertices * 6, vertices * 3, triangles * 6, clusters * lit_mesh.CLUSTER.size, nodes * lit_mesh.NODE.size, 0)
        for offset, size in zip(at, sizes):
            self.assertEqual(offset % 4, 0)
            self.assertLessEqual(offset + size, len(blob))

    def test_rebaking_a_flat_mesh_is_a_fixed_point(self):
        p, tris = grid(2)
        face_rgb = np.arange(len(tris) * 3).reshape(-1, 3) * 20
        with tempfile.TemporaryDirectory() as out:
            write_lit_mesh(out, "demo", p, None, tris, np.zeros(len(tris), dtype=int), face_rgb=face_rgb)
            entry = pathlib.Path(out) / "demo.mesh"
            rebake.main([str(entry)])
            once = entry.read_bytes()
            rebake.main([str(entry)])
            twice = entry.read_bytes()
        self.assertEqual(once, twice)

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


def lighting_lights(**changes):
    values = dict(sun=[-0.25, 1.0, 0.22], sun_disc_deg=1.2, sun_rays=4, sky_rays=8, sun_intensity=3.0,
                  sky_intensity=0.9, ambient=0.06)
    values.update(changes)
    return [
        {"type": "directional", "direction": values["sun"], "color": [1.0, 0.92, 0.78],
         "intensity": values["sun_intensity"], "disc_degrees": values["sun_disc_deg"], "rays": values["sun_rays"]},
        {"type": "sky", "color": [0.55, 0.68, 0.9], "intensity": values["sky_intensity"], "rays": values["sky_rays"]},
        {"type": "ambient", "color": [1.0, 1.0, 1.0], "intensity": values["ambient"]},
    ]


def walled_floors(offsets):
    """An 8x8 floor (two triangles) with a tall wall along one side, once per
    x offset: every copy sees the same sky, shadowed on one side."""
    meshes, positions, tris = [], [], []
    for ox in offsets:
        floor = trimesh.Trimesh([(ox, 0, 0), (ox, 0, 8), (ox + 8, 0, 8), (ox + 8, 0, 0)], [(0, 1, 2), (0, 2, 3)],
                                process=False)
        wall = trimesh.creation.box(extents=(1, 6, 8))
        wall.apply_translation((ox + 8.5, 3, 4))
        meshes += [floor, wall]
        tris.append(np.array(floor.faces) + 4 * len(positions))
        positions.append(np.array(floor.vertices))
    return np.concatenate(positions), np.concatenate(tris), RayMeshIntersector(trimesh.util.concatenate(meshes))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class FlatLightTests(unittest.TestCase):
    def colours(self, offsets, **kw):
        p, tris, intersector = walled_floors(offsets)
        grey = lambda points, spacing, m: np.full((len(points), 3), 0.3)
        lights = lighting_lights(sun=[0.8, 1.0, 0.0], sun_intensity=1.0, sky_intensity=1.0, ambient=0.02)
        return face_colours(p, tris, np.zeros(len(tris), dtype=int), [0], set(), grey, intersector, lights, 0.5, 0.35, **kw)

    def test_coplanar_faces_with_the_same_surroundings_get_the_same_colour(self):
        c = self.colours([0, 64])
        self.assertEqual(c[:2].tolist(), c[2:].tolist())
        self.assertNotEqual(c[0].tolist(), c[1].tolist(), "the wall must shade the two faces differently")

    def test_more_samples_per_face_converge(self):
        reference = self.colours([0], samples=256).astype(float)
        error = {n: np.abs(self.colours([0], samples=n) - reference).mean() for n in (1, 8, 64)}
        self.assertLess(error[64], error[8])
        self.assertLess(error[8], error[1])

    def test_small_faces_get_one_sample_and_large_faces_more(self):
        counts = adaptive_sample_counts(np.array([0.2, 1.0, 1.4, 3.0, 8.0]), 1.0, 16)
        self.assertEqual(counts.tolist(), [1, 1, 1, 3, 8])

    def test_the_adaptive_count_respects_its_minimum(self):
        self.assertEqual(adaptive_sample_counts(np.array([0.2, 1.0, 5.0]), 1.0, 8, 3).tolist(), [3, 3, 5])
        small = self.colours([0], samples="auto", sample_area=1e6, min_samples=4)
        self.assertEqual(small.tolist(), self.colours([0], samples=4).tolist())

    def test_the_adaptive_count_is_capped(self):
        self.assertEqual(adaptive_sample_counts(np.array([5.0, 500.0]), 1.0, 6).tolist(), [5, 6])

    def test_auto_samples_follow_the_face_area_and_stop_at_the_cap(self):
        # Two equal floor triangles: auto takes one sample each at the median area, and at a tiny
        # reference takes the cap, which the fixed count of the same size reproduces exactly.
        self.assertEqual(self.colours([0], samples="auto").tolist(), self.colours([0], samples=1).tolist())
        capped = self.colours([0], samples="auto", sample_area=1e-6, max_samples=3)
        self.assertEqual(capped.tolist(), self.colours([0], samples=3).tolist())

    def test_a_smooth_bake_draws_its_rays_as_before(self):
        floor = trimesh.Trimesh([(0, 0, 0), (0, 0, 8), (8, 0, 8), (8, 0, 0)], [(0, 1, 2), (0, 2, 3)], process=False)
        wall = trimesh.creation.box(extents=(1, 6, 8))
        wall.apply_translation((8.5, 3, 4))
        points = np.array([[1, 0, 1], [4, 0, 4], [7, 0, 2], [7.5, 0, 7.5], [2, 0, 6]], dtype=float)
        normals = np.tile([0.0, 1.0, 0.0], (5, 1))
        normals[4] = [0, -1, 0]
        got = light(points, normals, np.array([False, False, False, False, True]),
                    RayMeshIntersector(trimesh.util.concatenate([floor, wall])), lighting_lights(), 0.5, np.random.default_rng(7))
        # Recorded from the bake before flat faces shared their sky directions.
        want = [[3.4013203074259875, 3.290614682831909, 3.09012983979227], [3.3394453074259873, 3.214114682831909, 2.98887983979227], [3.277570307425987, 3.137614682831909, 2.88762983979227], [3.2156953074259875, 3.0611146828319087, 2.78637983979227], [3.3394453074259873, 3.214114682831909, 2.98887983979227]]
        np.testing.assert_allclose(got, want, rtol=0, atol=1e-12)


def box_inside(size):
    """A closed box whose triangles wind to face inward."""
    box = trimesh.creation.box(extents=(size, size, size))
    return np.array(box.vertices), np.array(box.faces)[:, [0, 2, 1]]


def bleed_scene():
    """A white floor with a saturated wall on it; material 0 is the floor."""
    floor = trimesh.Trimesh([(0, 0, 0), (0, 0, 40), (40, 0, 40), (40, 0, 0)], [(0, 1, 2), (0, 2, 3)], process=False)
    wall = trimesh.creation.box(extents=(1, 10, 40))
    wall.apply_translation((40.5, 5, 20))
    mesh = trimesh.util.concatenate([floor, wall])
    tri_mat = np.array([0] * len(floor.faces) + [1] * len(wall.faces))
    return np.array(mesh.vertices), np.array(mesh.faces), tri_mat, RayMeshIntersector(mesh)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class IndirectLightTests(unittest.TestCase):
    ONE_FLOOR_LIGHT = [{"type": "directional", "direction": [0.6, 1.0, 0.0], "color": [1, 1, 1], "intensity": 1.0,
                        "disc_degrees": 0.5, "rays": 4}]

    def cache(self, p, tris, tri_mat, intersector, albedo, lights, bounces, rays=128):
        return build_indirect_cache(p, tris, tri_mat, sorted(set(tri_mat.tolist())), set(),
                                    lambda points, spacing, material: np.tile(albedo[material], (len(points), 1)),
                                    intersector, lights, 0.01, SimpleNamespace(bounces=bounces, rays=rays, cache_samples=1),
                                    np.random.default_rng(3))

    def test_a_closed_diffuse_box_follows_the_bounce_series(self):
        p, tris = box_inside(10.0)
        intersector = RayMeshIntersector(trimesh.Trimesh(p, tris, process=False))
        lights = [{"type": "ambient", "color": [1, 1, 1], "intensity": 1.0}]
        a, inside = 0.5, np.array([[1.0, 2.0, -3.0], [-4.0, 0.5, 4.0]])
        for bounces in (1, 2, 3):
            cache = self.cache(p, tris, np.zeros(len(tris), dtype=int), intersector, {0: np.full(3, a)}, lights, bounces)
            got = gather_indirect(inside, np.tile([0.0, 1.0, 0.0], (2, 1)), intersector, cache, np.random.default_rng(5))
            np.testing.assert_allclose(got, np.full((2, 3), sum(a**k for k in range(1, bounces + 1))), rtol=1e-9)

    def test_colour_bleed_rises_toward_the_coloured_wall_and_only_with_indirect(self):
        p, tris, tri_mat, intersector = bleed_scene()
        albedo = {0: np.array([0.8, 0.8, 0.8]), 1: np.array([0.9, 0.05, 0.05])}
        cache = self.cache(p, tris, tri_mat, intersector, albedo, self.ONE_FLOOR_LIGHT, 2)
        points = np.array([[39.0, 0.0, 20.0], [20.0, 0.0, 20.0], [2.0, 0.0, 20.0]])
        up = np.tile([0.0, 1.0, 0.0], (3, 1))
        direct = light(points, up, np.zeros(3, dtype=bool), intersector, self.ONE_FLOOR_LIGHT, 0.01, np.random.default_rng(1))
        total = direct + gather_indirect(points, up, intersector, cache, np.random.default_rng(2))
        ratio = lambda radiance: (albedo[0] * radiance)[:, 0] / (albedo[0] * radiance)[:, 1]
        np.testing.assert_allclose(ratio(direct), 1.0)
        self.assertGreater(ratio(total)[0], 1.05)
        self.assertGreater(ratio(total)[0], ratio(total)[1])
        self.assertLess(abs(ratio(total)[2] - 1.0), 0.02)

    def test_a_blocked_ray_takes_the_nearer_surfaces_radiance(self):
        big = 1000.0
        quad = lambda y: [(-big, y, -big), (big, y, -big), (big, y, big), (-big, y, big)]
        p = np.array(quad(1.0) + quad(3.0))
        tris = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 6], [4, 6, 7]])
        intersector = RayMeshIntersector(trimesh.Trimesh(p, tris, process=False))
        cache = IndirectCache(np.array([[[1.0, 0.0, 0.0]] * 2 + [[0.0, 0.0, 1.0]] * 2]), 16, 0.01)
        got = gather_indirect(np.zeros((1, 3)), np.array([[0.0, 1.0, 0.0]]), intersector, cache, np.random.default_rng(4))
        self.assertEqual(got.tolist(), [[1.0, 0.0, 0.0]])

    def test_a_miss_adds_nothing(self):
        p = np.array([(-100.0, -1.0, -100.0), (100.0, -1.0, -100.0), (100.0, -1.0, 100.0)])
        intersector = RayMeshIntersector(trimesh.Trimesh(p, [[0, 1, 2]], process=False))
        cache = IndirectCache(np.ones((1, 1, 3)), 8, 0.01)
        got = gather_indirect(np.zeros((1, 3)), np.array([[0.0, 1.0, 0.0]]), intersector, cache, np.random.default_rng(1))
        self.assertEqual(got.tolist(), [[0.0, 0.0, 0.0]])

    def test_without_a_cache_the_gather_is_exactly_zero(self):
        got = gather_indirect(np.zeros((3, 3)), np.tile([0.0, 1.0, 0.0], (3, 1)), None, None, np.random.default_rng(1))
        self.assertEqual(got.tolist(), np.zeros((3, 3)).tolist())

    def test_zero_bounces_is_byte_identical_to_no_cache(self):
        p, tris, intersector = walled_floors([0])
        grey = lambda points, spacing, material: np.full((len(points), 3), 0.3)
        lights = lighting_lights(sun=[0.8, 1.0, 0.0], sun_intensity=1.0, sky_intensity=1.0, ambient=0.02)
        direct = face_colours(p, tris, np.zeros(len(tris), dtype=int), [0], set(), grey, intersector, lights, 0.5, 0.35)
        cache = build_indirect_cache(p, tris, np.zeros(len(tris), dtype=int), [0], set(), grey, intersector, lights, 0.5,
                                     SimpleNamespace(bounces=0, rays=1, cache_samples=1), np.random.default_rng(1))
        self.assertIsNone(cache)
        self.assertEqual(direct.tolist(), face_colours(p, tris, np.zeros(len(tris), dtype=int), [0], set(), grey,
                                                       intersector, lights, 0.5, 0.35, indirect_cache=cache).tolist())

    def test_equal_surroundings_get_equal_colours_with_indirect_light(self):
        p, tris, intersector = walled_floors([0, 64])
        grey = lambda points, spacing, material: np.full((len(points), 3), 0.3)
        lights = lighting_lights(sun=[0.8, 1.0, 0.0], sun_intensity=1.0, sky_intensity=1.0, ambient=0.02)
        mat = np.zeros(len(tris), dtype=int)
        cache = build_indirect_cache(p, tris, mat, [0], set(), grey, intersector, lights, 0.5,
                                     SimpleNamespace(bounces=2, rays=32, cache_samples=1), np.random.default_rng(1))
        c = face_colours(p, tris, mat, [0], set(), grey, intersector, lights, 0.5, 0.35, indirect_cache=cache).tolist()
        self.assertEqual(c[:2], c[2:])


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
            wq, _, wt, _, _ = weld_quantised(q, np.clip(np.rint(rgb), 0, 255).astype(np.int64), tris, double)
            self.assertEqual(canonical(mesh.pos, mesh.tris), canonical(wq, wt))

    def test_a_cluster_is_of_one_sidedness_and_both_kinds_are_there(self):
        for m, mesh in list(self.each()):
            self.assertEqual({c[6] for c in mesh.clusters}, {bool(d) for d in np.unique(m[3])})

    def test_the_tree_is_built_over_the_meshlets(self):
        for _, mesh in list(self.each()):
            self.assertEqual(sum(n["count"] for n in mesh.nodes if n["leaf"]), len(mesh.clusters))
            self.assertLess(len(mesh.nodes), len(mesh.clusters))

    def test_the_entry_reads_back_as_what_was_baked(self):
        for m, mesh in list(self.each()):
            with tempfile.TemporaryDirectory() as out:
                write_lit_mesh(out, "demo", *m)
                back = read_lit_mesh(pathlib.Path(out) / "demo.mesh")
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
                write_lit_mesh(out, "demo", *m)
                entry = pathlib.Path(out) / "demo.mesh"
                rebake.main([str(entry), "--meshlet-triangles", "24"])
                once = entry.read_bytes()
                rebake.main([str(entry), "--meshlet-triangles", "24"])
                twice = entry.read_bytes()
                back = read_lit_mesh(entry)
            self.assertEqual(once, twice)
            self.assertEqual(canonical(back.pos, back.tris), canonical(mesh.pos, mesh.tris))

if __name__ == "__main__":
    unittest.main()
