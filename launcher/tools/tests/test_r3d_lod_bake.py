"""A lit mesh baked with levels (r3d/lod/bake_levels.py through lit_mesh.py): level 0 is the plain bake,
the groups keep the format's ordering after rounding to ticks, coarse vertices only move outward, and the
engine frame mirrors the group records."""

import math
import pathlib
import sys
import tempfile
import unittest

import numpy as np
from scipy.spatial.transform import Rotation

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from asset import engine_frame
from r3d import lit_mesh, mesh_asset, lod_asset
from r3d.lod.bake_levels import bake_lit_mesh
from r3d.geometry import closest_point_on_triangles
from r3d.lod import pick
from r3d.lit_mesh import MAX_TRIANGLES, MAX_VERTICES
from r3d.lod.bake_levels import INT16_MAX, push_outward, quantised_groups, within_format
from r3d.mesh_asset import LOD_ENCLOSE_MARGIN, LOD_NONE
import test_r3d_lod

GRID = 24
# A grid whose level 0 fits the format but whose levels together need over MAX_VERTICES vertices.
CROWDED_GRID = 150
# Every fourth triangle row is two-sided, so both sides get a hierarchy and share a border.
DOUBLE_EVERY = 4
FOCAL_PX = 64.0
# The picture the coverage cap is a share of; the reference rule has no cap.
HEIGHT_PX = 2 * int(FOCAL_PX)
NEAR = 1.0
# The placement: a turn and one scale, so the device's bound on the largest scale is exact.
TURN = Rotation.from_euler("xyz", [20, -35, 50], degrees=True).as_matrix()
SCALE = 1.5
SHIFT = np.array([40.0, -30.0, 70.0])
# Eye distances from the mesh's middle, in its placed extents, along one direction.
EYES = np.geomspace(0.5, 500.0, 48)
# Projected errors this near the threshold may round either way between float32 and float64.
BOUNDARY_PX = 1e-3
SIDE = 4  # the push patch: SIDE x SIDE vertices
SPACING = 10  # ticks between the patch's vertices, so a miss shows past rounding
BUMP = 3 * SPACING  # how far the patch's one raised vertex stands off its plane
# How far rounding the pushed corners to ticks can leave a fine vertex outside.
ROUNDING_TICKS = 1.0


def wavy_grid(n=GRID):
    """An n x n quad grid, bent so simplifying it costs error, coloured by position."""
    points, triangles = test_r3d_lod.wavy_grid(n, 3.0, (5.0, 7.0))
    rgb = np.stack([points[:, 0] * 10, points[:, 1] * 10, np.full(len(points), 100)], axis=1).astype(int)
    double = (np.arange(len(triangles)) % n // (n // DOUBLE_EVERY) == 0).astype(int)
    return points, rgb, triangles, double


def read_back(mesh):
    with tempfile.TemporaryDirectory() as folder:
        path = pathlib.Path(folder) / "mesh.bundle"
        return lit_mesh.read_lit_mesh(lit_mesh.mesh_blob(mesh), lit_mesh.lod_blob(mesh))


def corner_set(welded):
    """Each triangle as its corners' (position, colour) from the lowest corner, with its side."""
    pos, rgb, tris, double = welded[:4]
    out = set()
    for t, d in zip(tris, double):
        corners = [(*pos[v], *rgb[v]) for v in t]
        k = corners.index(min(corners))
        out.add((tuple(corners[k:] + corners[:k]), int(d)))
    return out


class BakedLevelsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        points, rgb, triangles, double = wavy_grid()
        cls.levelled = read_back(bake_lit_mesh(points, rgb, triangles, double, lod=True))
        cls.plain = read_back(lit_mesh.bake_lit_mesh(points, rgb, triangles, double))

    def test_level_zero_is_the_plain_bake_with_its_colours(self):
        self.assertEqual(corner_set(lit_mesh.finest_triangles(self.plain)),
                         corner_set(lit_mesh.finest_triangles(self.levelled)))

    def test_a_finest_cluster_keeps_its_triangles_in_the_plain_bakes_order(self):
        """However the clusterer orders a cluster's triangles, level 0 draws them as the plain bake does."""
        from unittest import mock
        from r3d.lod import bake_levels, levels

        def turned_around(*args, **kwargs):
            built = levels.build_levels(*args, **kwargs)
            indices = built.indices.copy()
            for first, count in zip(built.cluster_first, built.cluster_triangles):
                run = indices[first:first + 3 * count].reshape(-1, 3)
                indices[first:first + 3 * count] = run[::-1].reshape(-1)
            return built._replace(indices=indices)

        points, rgb, triangles, double = wavy_grid()
        with mock.patch.object(bake_levels, "build_levels", turned_around):
            levelled = read_back(bake_lit_mesh(points, rgb, triangles, double, lod=True))

        def shape(pos, t):
            return tuple(sorted(tuple(pos[v]) for v in t))

        plain = lit_mesh.finest_triangles(self.plain)
        at = {shape(plain[0], t): row for row, t in enumerate(plain[2])}
        level0 = lit_mesh.finest(levelled)
        for c, (_, _, tbase, tcount, _, _, _) in enumerate(levelled.clusters):
            if level0[c]:
                rows = [at[shape(levelled.pos, t)] for t in levelled.tris[tbase:tbase + tcount]]
                self.assertEqual(rows, sorted(rows), f"cluster {c}")

    def test_coarser_levels_are_baked_but_left_out_of_the_finest_triangles(self):
        finest = lit_mesh.finest(self.levelled)
        self.assertTrue(finest.any() and not finest.all())
        self.assertGreater(len(self.levelled.groups), 1)
        self.assertEqual(len(self.plain.tris), len(lit_mesh.finest_triangles(self.levelled)[2]))
        self.assertGreater(len(self.levelled.tris), len(self.plain.tris))

    def test_the_root_holds_the_finest_clusters_tree_then_the_coarse_clusters_tree(self):
        nodes, finest = self.levelled.nodes, lit_mesh.finest(self.levelled)

        def under(node):
            n = nodes[node]
            if n["leaf"]:
                return list(range(n["first"], n["first"] + n["count"]))
            return [c for child in range(n["first"], n["first"] + n["count"]) for c in under(child)]

        root = nodes[0]
        self.assertEqual((False, 2), (root["leaf"], root["count"]))
        self.assertEqual(sorted(np.flatnonzero(finest)), sorted(under(root["first"])))
        self.assertEqual(sorted(np.flatnonzero(~finest)), sorted(under(root["first"] + 1)))

    def test_the_coarse_clusters_follow_level_zero_in_the_order_of_their_own_group(self):
        finest = lit_mesh.finest(self.levelled)
        first = int(np.flatnonzero(~finest)[0])
        self.assertTrue(finest[:first].all() and not finest[first:].any())
        owns = [own for own, _ in self.levelled.lod[first:]]
        self.assertEqual(sorted(owns), owns)

    def test_every_group_holds_clusters_and_has_clusters_simplified_from_it(self):
        owned = {own for own, _ in self.levelled.lod} - {LOD_NONE}
        parents = {parent for _, parent in self.levelled.lod} - {LOD_NONE}
        self.assertEqual(set(range(len(self.levelled.groups))), owned)
        self.assertLessEqual(owned - parents, set())

    def test_the_engine_frame_mirrors_each_group_centre_and_keeps_its_radius_and_error(self):
        blob = lit_mesh.lod_blob(self.levelled)
        mirrored = engine_frame.to_engine(lod_asset.TYPE, blob)
        _, groups, clusters = lod_asset.HEADER.unpack_from(blob)
        groups_at = lod_asset.HEADER.size + clusters * mesh_asset.CLUSTER_LOD.size
        for index in range(groups):
            at = groups_at + index * mesh_asset.LOD_GROUP.size
            *centre, radius, error = mesh_asset.LOD_GROUP.unpack_from(blob, at)
            self.assertEqual((*engine_frame._mirror_position(centre), radius, error),
                             mesh_asset.LOD_GROUP.unpack_from(mirrored, at))


def lens_rows(eye, target, model, shift):
    """The lens matrix the device builds: position ticks to lens space, the focal length on its first two rows."""
    right, up, forward = pick_basis(np.asarray(target) - eye)
    view = np.array([right * FOCAL_PX, up * FOCAL_PX, forward])
    rows = np.zeros((4, 4))
    rows[:3, :3] = view @ model
    rows[:3, 3] = view @ (shift - eye)
    rows[3, 3] = 1.0
    return rows


def pick_basis(forward):
    from r3d.poses import camera_basis
    return camera_basis(forward)


def reference_drawn(lod, groups, eye, model, shift, tau):
    """pick.py's rule at `tau` over the same groups, placed; and whether each cluster sits near a switch."""
    centres = groups[:, :3] @ model.T + shift
    spheres = np.concatenate([centres, groups[:, 3:4] * SCALE], axis=1)
    px = pick.projected_px(spheres, groups[:, 4] * SCALE, eye, FOCAL_PX, NEAR)
    own = np.array([0.0 if o == LOD_NONE else px[o] for o, _ in lod])
    parent = np.array([np.inf if p == LOD_NONE else px[p] for _, p in lod])
    view = pick.View(np.ones(len(lod)), own, parent, own == 0)
    near = (np.abs(own - tau) < BOUNDARY_PX) | (np.abs(parent - tau) < BOUNDARY_PX)
    return pick.drawn(view, tau), near


class DevicePickTest(unittest.TestCase):
    """The device's pick, from clear bits, draws what pick.py's rule draws at the threshold a group turns coarse at."""

    def test_the_device_pick_matches_the_reference_from_clear_bits(self):
        if not hasattr(BakedLevelsTest, "levelled"):
            BakedLevelsTest.setUpClass()
        mesh = BakedLevelsTest.levelled
        model = TURN * SCALE
        middle = mesh.pos.mean(axis=0) @ model.T + SHIFT
        extent = np.ptp(mesh.pos, axis=0).max() * SCALE
        direction = np.array([0.3, 0.5, -1.0]) / np.linalg.norm([0.3, 0.5, -1.0])
        tau = pick.TOLERANCE_PX - pick.HYSTERESIS_PX
        seen = set()
        for distance in EYES:
            eye = middle + direction * distance * extent
            _, drawn = pick.device_pick(mesh.lod, mesh.groups, lens_rows(eye, middle, model, SHIFT),
                                        (FOCAL_PX, FOCAL_PX), NEAR, HEIGHT_PX, coverage_max=math.inf)
            expected, near = reference_drawn(mesh.lod, mesh.groups, eye, model, SHIFT, tau)
            np.testing.assert_array_equal(expected[~near], drawn[~near])
            seen.update(np.flatnonzero(drawn).tolist())
        finest = set(np.flatnonzero(lit_mesh.finest(mesh)).tolist())
        self.assertTrue(seen & finest and seen - finest, "the path draws only one kind of level")


class QuantisedGroupsTest(unittest.TestCase):
    SPHERES = np.array([[0.4, 0.4, 0.0, 2.2], [5.6, 0.0, 0.0, 2.0], [3.0, 0.2, 0.0, 6.0]])
    ERRORS = np.array([0.3, 0.5, 0.2])
    SOURCES = [[], [], [0, 1]]

    def test_rounded_groups_hold_the_float_spheres_and_their_sources_with_the_margin(self):
        rows, kept = quantised_groups(self.SPHERES, self.ERRORS, self.SOURCES)
        self.assertTrue(kept.all())
        for g, sphere in enumerate(self.SPHERES):
            reach = np.linalg.norm(sphere[:3] - rows[g, :3]) + sphere[3]
            self.assertGreaterEqual(rows[g, 3], reach)
            self.assertGreaterEqual(rows[g, 4], self.ERRORS[g])
        for g, sources in enumerate(self.SOURCES):
            for s in sources:
                room = rows[g, 3] - rows[s, 3] - LOD_ENCLOSE_MARGIN
                self.assertLessEqual(((rows[g, :3] - rows[s, :3]) ** 2).sum(), room * room)
                self.assertGreaterEqual(rows[g, 4], rows[s, 4])

    def test_a_group_past_int16_is_dropped_with_every_group_above_it(self):
        spheres = self.SPHERES.copy()
        spheres[1, 0] = INT16_MAX + 1.0
        rows, kept = quantised_groups(spheres, self.ERRORS, self.SOURCES)
        self.assertEqual([True, False, False], kept.tolist())

    def test_groups_join_finest_first_and_stop_at_the_first_past_a_format_count(self):
        rows, kept = quantised_groups(self.SPHERES, self.ERRORS, self.SOURCES)
        cost = np.array([[10, 4, 1], [10, 4, 1], [6, 2, 1]])
        total = cost.sum(axis=0)

        def joined(vertices, triangles):
            return within_format(rows, kept, cost, np.array([MAX_VERTICES - vertices, MAX_TRIANGLES - triangles, 0]))

        self.assertEqual([True, True, True], joined(total[0], total[1]).tolist())
        self.assertEqual([True, True, False], joined(total[0] - 1, total[1]).tolist())
        self.assertEqual([True, True, False], joined(total[0], total[1] - 1).tolist())
        self.assertEqual([False, False, False], joined(cost[0, 0] - 1, total[1]).tolist())


class CrowdedLevelsTest(unittest.TestCase):
    def test_a_mesh_whose_levels_pass_the_vertex_count_bakes_the_levels_that_fit(self):
        mesh = read_back(bake_lit_mesh(*wavy_grid(CROWDED_GRID), lod=True))
        self.assertLessEqual(len(mesh.pos), MAX_VERTICES)
        self.assertLessEqual(len(mesh.tris), MAX_TRIANGLES)
        finest = lit_mesh.finest(mesh)
        self.assertTrue(len(mesh.groups) and not finest.all(), "no coarser level was kept")


class PushOutwardTest(unittest.TestCase):
    """A SIDE x SIDE patch in z = 0 with one vertex raised BUMP, simplified to a fan around an inner vertex."""

    def setUp(self):
        x, y = np.meshgrid(np.arange(SIDE) * SPACING, np.arange(SIDE) * SPACING)
        self.pos = np.stack([x.ravel(), y.ravel(), np.zeros(SIDE * SIDE)], axis=1).astype(np.int64)
        self.raised = 1 * SIDE + 2
        self.pos[self.raised, 2] = BUMP
        corner = (np.arange(SIDE - 1)[None, :] + np.arange(SIDE - 1)[:, None] * SIDE).ravel()
        self.fine = np.concatenate([np.stack([corner, corner + 1, corner + SIDE + 1], 1),
                                    np.stack([corner, corner + SIDE + 1, corner + SIDE], 1)])
        self.hub = 1 * SIDE + 1
        last = SIDE - 1
        loop = ([i for i in range(SIDE)] + [i * SIDE + last for i in range(1, SIDE)]
                + [last * SIDE + i for i in range(last - 1, -1, -1)] + [i * SIDE for i in range(last - 1, 0, -1)])
        self.coarse = np.array([(self.hub, loop[i], loop[(i + 1) % len(loop)]) for i in range(len(loop))])
        self.border = set(loop)

    def pushed(self, cap):
        moved = push_outward(self.fine, self.pos[self.fine], self.coarse, cap)
        return {int(v): tuple(p) for t, corners in zip(self.coarse, moved) for v, p in zip(t, corners)}

    def test_the_coarse_fan_covers_every_fine_vertex_and_the_border_stays(self):
        moved = self.pushed(cap=10 * BUMP)
        for v, p in moved.items():
            if v in self.border:
                self.assertEqual(tuple(self.pos[v]), p)
        self.assertGreaterEqual(moved[self.hub][2], BUMP)
        corners = np.array([[moved[int(v)] for v in t] for t in self.coarse], dtype=np.float64)
        a, b, c = corners[:, 0], corners[:, 1], corners[:, 2]
        normal = np.cross(b - a, c - a)
        unit = normal / np.linalg.norm(normal, axis=1, keepdims=True)
        for point in self.pos.astype(np.float64):
            rows = np.repeat(point[None], len(corners), axis=0)
            bary, distance = closest_point_on_triangles(rows, a, b, c)
            k = int(np.argmin(distance))
            closest = bary[k, 0] * a[k] + bary[k, 1] * b[k] + bary[k, 2] * c[k]
            self.assertLessEqual(float((point - closest) @ unit[k]), ROUNDING_TICKS)

    def test_the_push_stops_at_the_cap(self):
        self.assertEqual((*self.pos[self.hub, :2], 1), self.pushed(cap=1)[self.hub])


if __name__ == "__main__":
    unittest.main()
