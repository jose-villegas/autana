"""Checks the baked cluster levels on two meshes built here, a subdivided
sphere (closed) and a bumpy sheet (open, with two materials), and on the
Sponza bake in the tree when it is there: meshlet sizes, every finest
triangle drawn once, no vertex ever moving between levels, no crack in any
cut of the levels, errors that only grow up the hierarchy, cones that hold,
and the evaluation tool's counts. Skipped where the pinned environment
(tools/r3d/requirements.txt) is not installed."""

import collections
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import lod_eval
    from r3d.lit_mesh import bake_lit_mesh, finest_triangles, read_lit_mesh, weld_quantised, write_lit_mesh
    from r3d.rebake import main as rebake
except ImportError:
    np = None

SPONZA = pathlib.Path(__file__).resolve().parents[2] / "main" / "apps" / "render_lab" / "sponza_lite_mesh_generated.c"
MESHLET = 32


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
    """An n by n bumpy sheet facing +z, the two halves apart in sidedness."""
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


def bake(mesh, **options):
    p, rgb, tris, double = mesh
    return bake_lit_mesh(p, rgb, tris, double, leaf_triangles=200, max_depth=8, meshlet_triangles=MESHLET,
                         partition_size=4, with_lod=True, **options)


def canonical(pos, tris):
    """Each triangle as its three positions turned to start at the smallest."""
    out = []
    for t in tris:
        corners = [tuple(int(x) for x in pos[i]) for i in t]
        k = corners.index(min(corners))
        out.append(tuple(corners[k:] + corners[:k]))
    return collections.Counter(out)


def edges_of_cut(level, mask):
    """The directed edges, by position, of the clusters in the mask."""
    edges = collections.Counter()
    for k in np.flatnonzero(mask):
        pos, _ = lod_eval.cluster_triangles(level, k)
        q = np.round(pos * level.scale).astype(np.int64)
        for tri in q:
            for i in range(3):
                edges[(tuple(tri[i]), tuple(tri[(i + 1) % 3]))] += 1
    return edges


def unmatched(edges):
    return [e for e in edges if edges[e] != edges.get((e[1], e[0]), 0)]


def eyes(mesh, count, seed):
    rng = np.random.default_rng(seed)
    centre = mesh.pos.mean(axis=0) / mesh.position_scale
    span = np.ptp(mesh.pos, axis=0).max() / mesh.position_scale
    for _ in range(count):
        d = rng.normal(size=3)
        yield centre + d / np.linalg.norm(d) * span * rng.uniform(0.7, 6.0), -d


def camera(eye, forward):
    return lod_eval.Camera(eye, forward + [0.0, 0.3, 0.0], 184, 224, 0.62, 1.0)


def per_mesh(test):
    """Runs a test once on each mesh built here, as a subtest of its own."""

    def run(self):
        for name in self.inputs:
            with self.subTest(mesh=name):
                test(self, self.inputs[name], self.baked[name])

    run.__name__ = test.__name__
    return run


@unittest.skipIf(np is None, "the r3d environment is not installed")
class MeshTests(unittest.TestCase):
    """Every check that holds for any mesh, run on each of the meshes built here."""

    @classmethod
    def setUpClass(cls):
        cls.inputs = {"sphere": sphere(3), "sheet": sheet(28)}
        cls.baked = {name: bake(m) for name, m in cls.inputs.items()}

    @per_mesh
    def test_meshlets_stay_within_their_size_and_most_are_nearly_full(self, _m, mesh):
        sizes = np.array([c[3] for c in mesh.clusters])
        vertices = np.array([c[1] for c in mesh.clusters])
        self.assertLessEqual(sizes.max(), MESHLET)
        self.assertLessEqual(vertices.max(), MESHLET)
        self.assertGreaterEqual(np.percentile(sizes, 50), MESHLET // 2)
        for c in mesh.lod.clusters:
            self.assertLessEqual(c[3], MESHLET)

    @per_mesh
    def test_every_finest_triangle_is_in_one_cluster_and_no_other(self, m, mesh):
        p, rgb, tris, double = m
        q = np.round(p * mesh.position_scale).astype(np.int64)
        wq, _, wt, _ = weld_quantised(q, np.clip(np.rint(rgb), 0, 255).astype(np.int64), tris, double)
        self.assertEqual(canonical(mesh.pos, mesh.tris), canonical(wq, wt))

    @per_mesh
    def test_a_cluster_is_single_sided_or_double_sided_never_both(self, m, mesh):
        flagged = 0
        for _, _, tbase, tcount, _, _, double in mesh.clusters + mesh.lod.clusters:
            flagged += double
        self.assertEqual(bool(flagged), bool(m[3].any()))
        finest = {c[6] for c in mesh.clusters}
        self.assertEqual(finest, {bool(d) for d in np.unique(m[3])})

    @per_mesh
    def test_a_vertex_never_moves_or_changes_colour_between_levels(self, m, mesh):
        copies = {}
        for level in (mesh, mesh.lod):
            for s, pos, rgb in zip(level.source, level.pos, level.rgb):
                copies.setdefault(int(s), set()).add((tuple(pos), tuple(rgb)))
        self.assertTrue(all(len(v) == 1 for v in copies.values()), "a vertex has two positions or colours")

    @per_mesh
    def test_no_cut_of_the_levels_opens_a_crack(self, m, mesh):
        level = lod_eval.Level(mesh)
        outline = np.abs(m[0][:, :2]).max()
        closed = not unmatched(edges_of_cut(level, level.level == 0))
        mixed = 0
        for eye, forward in eyes(mesh, 12, 7):
            for tolerance in (0.25, 1.0, 4.0):
                cut = lod_eval.cut(level, camera(eye, forward), tolerance)
                mixed += len(set(level.level[cut])) > 1
                for a, b in unmatched(edges_of_cut(level, cut)):
                    if closed:
                        self.fail("the mesh is closed but a cut has an open edge")
                    sides = [abs(abs(v[0]) / mesh.position_scale - outline) < 1e-6 or
                             abs(abs(v[1]) / mesh.position_scale - outline) < 1e-6 for v in (a, b)]
                    self.assertTrue(all(sides), "an open edge inside the sheet")
        self.assertGreater(mixed, 0, "no cut mixed levels, so nothing was tested")

    @per_mesh
    def test_a_cut_holds_the_surface_once(self, _m, mesh):
        level = lod_eval.Level(mesh)
        fine = mesh.tris.shape[0]
        for eye, forward in eyes(mesh, 6, 3):
            cut = lod_eval.cut(level, camera(eye, forward), 0.0)
            self.assertEqual(int(level.count[cut].sum()), fine)
            self.assertTrue(np.all(level.level[cut] == 0))

    @per_mesh
    def test_errors_only_grow_and_a_parent_sphere_holds_its_child(self, _m, mesh):
        for r in mesh.records:
            self.assertLessEqual(r["self"][2], r["parent"][2])
            (cc, cr, _), (pc, pr, _) = r["self"], r["parent"]
            self.assertLessEqual(np.linalg.norm(cc - pc) + cr, pr + 2, "a parent sphere misses its child")
        top = [r for r in mesh.records if r["parent"][2] >= 1e30]
        self.assertTrue(top, "nothing is at the top")
        errors = [r["self"][2] for r in mesh.records if r["level"] > 0]
        self.assertTrue(all(e > 0 for e in errors))

    @per_mesh
    def test_a_cone_holds_every_normal_and_never_culls_what_faces_the_eye(self, _m, mesh):
        level = lod_eval.Level(mesh)
        clusters = mesh.clusters + mesh.lod.clusters
        culled = 0
        for eye, forward in eyes(mesh, 8, 11):
            back = ~lod_eval.facing(camera(eye, forward), level)
            for k in np.flatnonzero(back):
                self.assertFalse(clusters[k][6], "a double sided cluster was culled")
                pos, _ = lod_eval.cluster_triangles(level, k)
                normal = np.cross(pos[:, 1] - pos[:, 0], pos[:, 2] - pos[:, 0])
                towards = pos.mean(axis=1) - eye
                self.assertTrue(np.all(np.sum(normal * towards, axis=1) >= -1e-6 * np.abs(normal).max()),
                                "a triangle facing the eye was culled with its cluster")
                culled += 1
        self.assertGreater(culled, 0, "no cluster was culled, so nothing was tested")

    @per_mesh
    def test_the_evaluation_counts_add_up(self, _m, mesh):
        level = lod_eval.Level(mesh)
        for eye, forward in eyes(mesh, 4, 5):
            cam = camera(eye, forward)
            for tolerance in (0.0, 1.0, 8.0, 1e9):
                r = lod_eval.evaluate(level, cam, tolerance, images=True)
                picked = lod_eval.pick(level, cam, tolerance)
                self.assertEqual(r["triangles_lod"], int(level.count[picked].sum()))
                self.assertEqual(r["clusters_lod"], int(picked.sum()))
                self.assertEqual(r["clusters_coarser"], int((picked & (level.level > 0)).sum()))
                self.assertLessEqual(r["clusters_coarser"], r["clusters_lod"])
                self.assertLessEqual(r["drawn_finest"], r["triangles_finest"])
                self.assertLessEqual(r["drawn_lod"], r["triangles_lod"])
                if tolerance == 0.0:
                    self.assertEqual(r["clusters_coarser"], 0)
                    self.assertEqual(r["triangles_lod"], r["triangles_finest"])
                    self.assertEqual(r["differ"], 0.0)

    @per_mesh
    def test_a_coarser_cut_draws_fewer_triangles_and_differs_from_the_finest(self, _m, mesh):
        level = lod_eval.Level(mesh)
        eye, forward = next(eyes(mesh, 1, 2))
        cam = camera(eye, forward)
        fine = lod_eval.evaluate(level, cam, 0.0)["triangles_lod"]
        coarse = lod_eval.evaluate(level, cam, 1e9)["triangles_lod"]
        self.assertLess(coarse, fine)

    @per_mesh
    def test_the_c_data_reads_back_as_what_was_baked(self, m, mesh):
        with tempfile.TemporaryDirectory() as out:
            write_lit_mesh(out, "demo", *m, ["test"], 200, 8, meshlet_triangles=MESHLET, partition_size=4, with_lod=True)
            back = read_lit_mesh(str(pathlib.Path(out) / "demo_mesh_generated.c"))
        self.assertTrue(np.array_equal(back.pos, mesh.pos))
        self.assertTrue(np.array_equal(back.tris, mesh.tris))
        self.assertTrue(np.array_equal(back.lod.tris, mesh.lod.tris))
        self.assertEqual(len(back.nodes), len(mesh.nodes))
        self.assertEqual(back.records[-1]["level"], mesh.records[-1]["level"])
        self.assertAlmostEqual(back.records[0]["parent"][2], mesh.records[0]["parent"][2], places=3)
        self.assertTrue(np.array_equal(back.records[0]["cone"][0], mesh.records[0]["cone"][0]))

    @per_mesh
    def test_rebaking_keeps_the_finest_triangles(self, m, mesh):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            write_lit_mesh(first, "demo", *m, ["test"], 200, 8, meshlet_triangles=MESHLET, partition_size=4, with_lod=True)
            rebake([str(pathlib.Path(first) / "demo_mesh_generated.c"), "--out-dir", second,
                    "--meshlet-triangles", "48", "--partition-size", "6", "--lod"])
            back = read_lit_mesh(str(pathlib.Path(second) / "demo_mesh_generated.c"))
        self.assertEqual(canonical(back.pos, back.tris), canonical(mesh.pos, mesh.tris))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class DefaultTests(unittest.TestCase):
    """What is emitted when the levels are not asked for."""

    def test_a_mesh_is_finest_only_with_cones_unless_levels_are_asked_for(self):
        p, rgb, tris, double = sphere(2)
        with tempfile.TemporaryDirectory() as out:
            mesh = write_lit_mesh(out, "demo", p, rgb, tris, double, ["test"], 200, 8, meshlet_triangles=MESHLET)
            text = (pathlib.Path(out) / "demo_mesh_generated.c").read_text()
            back = read_lit_mesh(str(pathlib.Path(out) / "demo_mesh_generated.c"))
        self.assertIsNone(mesh.lod)
        self.assertEqual(mesh.records, [])
        self.assertNotIn("_lod", text)
        self.assertIn("demo_cones, NULL,", text)
        self.assertEqual(len(back.cones), len(back.clusters))
        self.assertTrue(any(c[1] < 127 for c in back.cones), "no cluster of a sphere has a usable cone")
        level = lod_eval.Level(back)
        cam = camera(*next(eyes(back, 1, 4)))
        self.assertTrue(np.array_equal(lod_eval.pick(level, cam, 1.0), lod_eval.finest(level, cam)))

    def test_a_poses_file_gives_its_size_lens_and_poses(self):
        size, lens, poses = lod_eval.read_poses("# c\nsize 4 6\nlens 0.5 2\npose 1 2 3 4 5 6 # x\n")
        self.assertEqual((size, lens, len(poses)), ((4, 6), (0.5, 2.0), 1))
        self.assertEqual(poses[0][1].tolist(), [4.0, 5.0, 6.0])
        with self.assertRaises(SystemExit):
            lod_eval.read_poses("pose 1 2\n")


@unittest.skipIf(np is None, "the r3d environment is not installed")
class TreeMeshTests(unittest.TestCase):
    def test_a_baked_mesh_in_the_tree_keeps_its_levels_crack_free(self):
        if not SPONZA.exists():
            self.skipTest("no baked mesh in the tree")
        with tempfile.TemporaryDirectory() as out:
            rebake([str(SPONZA), "--out-dir", out, "--lod"])
            mesh = read_lit_mesh(str(pathlib.Path(out) / SPONZA.name))
        self.assertIsNotNone(mesh.lod, "the baked mesh has no coarser levels")
        level = lod_eval.Level(mesh)
        centre = (mesh.pos.min(axis=0) + mesh.pos.max(axis=0)) / 2.0 / mesh.position_scale
        rng = np.random.default_rng(1)
        for _ in range(6):
            eye = centre + rng.normal(size=3) * 900.0
            cam = lod_eval.Camera(eye, centre - eye + [0.0, 1.0, 0.0], 184, 224, 0.62, 6.0)
            cut = lod_eval.cut(level, cam, 4.0)
            edges = edges_of_cut(level, cut)
            fine = edges_of_cut(level, level.level == 0)
            self.assertLessEqual(len(unmatched(edges)), len(unmatched(fine)), "a cut opens more edges than the mesh has")

    def test_the_baked_finest_level_is_the_whole_mesh(self):
        if not SPONZA.exists():
            self.skipTest("no baked mesh in the tree")
        mesh = read_lit_mesh(str(SPONZA))
        pos, _, tris, _ = finest_triangles(mesh)
        self.assertEqual(len(tris), len(mesh.tris))
        self.assertTrue(len(mesh.clusters) < len(mesh.tris))


if __name__ == "__main__":
    unittest.main()
