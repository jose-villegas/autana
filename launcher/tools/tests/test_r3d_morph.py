"""LMOR geomorph targets: the sibling codec, its engine frame, the delta
quantiser, the coarse-surface targets and the bake that applies them."""
import pathlib
import struct
import sys
import unittest
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from asset import engine_frame
from build.c_header import read_define
from r3d import lit_mesh, mesh_asset, mesh_bundle, morph_asset
from r3d.geometry import closest_point_on_triangles
from r3d.lod import morph
from r3d.lod.bake_levels import bake_lit_mesh, border_vertices
from r3d.lod.morph import KEPT_SLACK_TICKS, kept_corners, landing_weights, read_targets
from r3d.mesh_asset import LOD_NONE
from test_r3d_lod_bake import wavy_grid

AXES = morph_asset.AXES
# A float32-free bound: rounding a point on a triangle to ticks moves it half a tick per axis.
ROUNDING_TICKS = np.sqrt(AXES) / 2.0 + 1e-9
Z_AXIS = 2
TWO_CLUSTER_SHIFTS = [0, 3]
THREE_ROWS = [1, -2, 3, -4, 5, -6, 7, 8, -9]
WHOLE = morph_asset.WEIGHT_WHOLE
THREE_LANDING = [(0, 10, 20), (1, WHOLE, 0), (2, 0, WHOLE)]


def entry(shifts=(0,), deltas=(1, 2, 3), landing=None):
    return morph_asset.encode(list(shifts), list(deltas), landing)


def patched_header(blob, version=None, flags=None, clusters=None, vertices=None):
    _, f, c, v = morph_asset.HEADER.unpack_from(blob)
    head = morph_asset.HEADER.pack(morph_asset.VERSION if version is None else version,
                                   f if flags is None else flags, c if clusters is None else clusters,
                                   v if vertices is None else vertices)
    return head + blob[morph_asset.HEADER.size:]


class MorphCodecTests(unittest.TestCase):
    def test_round_trip_without_landing(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS)
        shifts, deltas, landing = morph_asset.decode(blob)
        self.assertEqual((TWO_CLUSTER_SHIFTS, THREE_ROWS, None), (shifts, deltas, landing))

    def test_round_trip_with_landing(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS, THREE_LANDING)
        shifts, deltas, landing = morph_asset.decode(blob)
        self.assertEqual((TWO_CLUSTER_SHIFTS, THREE_ROWS, THREE_LANDING), (shifts, deltas, landing))
        plain = len(entry(TWO_CLUSTER_SHIFTS, THREE_ROWS))
        self.assertEqual(plain + len(THREE_LANDING) * morph_asset.LANDING_SIZE, len(blob))

    def test_landing_constants_come_from_the_c_header(self):
        self.assertEqual(morph_asset.LANDING, read_define(morph_asset.OWNER, "LMOR_LANDING"))
        self.assertEqual(morph_asset.LANDING_SIZE, read_define(morph_asset.OWNER, "LMOR_LANDING_SIZE"))
        self.assertEqual(morph_asset.LANDING_SIZE, morph_asset.LANDING_POINT.size)
        self.assertEqual(WHOLE, read_define(morph_asset.OWNER, "LMOR_WEIGHT_WHOLE"))

    def test_landing_weights_that_exceed_the_whole_are_refused(self):
        morph_asset.encode([0], [0, 0, 0], [(0, WHOLE - 1, 1)])
        with self.assertRaises(ValueError):
            morph_asset.encode([0], [0, 0, 0], [(0, WHOLE, 1)])

    def test_layout_constants_come_from_the_c_header(self):
        self.assertEqual(morph_asset.HEADER.size, read_define(morph_asset.OWNER, "LMOR_HEADER_SIZE"))
        self.assertEqual(morph_asset.name_for_mesh("room"), "room.morph")
        self.assertEqual(morph_asset.SHIFT_MAX, read_define(morph_asset.OWNER, "LMOR_SHIFT_MAX"))

    def test_decode_rejects_sizes_that_do_not_match_the_counts(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS, THREE_LANDING)
        for bad in (blob[:morph_asset.HEADER.size - 1], blob[:-1], blob + bytes(morph_asset.ALIGN),
                    patched_header(blob, vertices=7), patched_header(blob, clusters=9)):
            with self.subTest(size=len(bad)), self.assertRaises(ValueError):
                morph_asset.decode(bad)

    def test_decode_rejects_unknown_flags_and_versions(self):
        blob = entry()
        for bad in (patched_header(blob, flags=morph_asset.LANDING << 1),
                    patched_header(blob, flags=morph_asset.LANDING | (morph_asset.LANDING << 1)),
                    patched_header(blob, version=morph_asset.VERSION + 1)):
            with self.subTest(header=bad[:morph_asset.HEADER.size]), self.assertRaises(ValueError):
                morph_asset.decode(bad)

    def test_a_flag_for_a_landing_part_the_entry_lacks_is_a_size_mismatch(self):
        with self.assertRaises(ValueError):
            morph_asset.decode(patched_header(entry(), flags=morph_asset.LANDING))

    def test_deltas_below_the_symmetric_minimum_are_refused(self):
        self.assertEqual(morph_asset.DELTA_MIN, -morph_asset.DELTA_MAX)
        morph_asset.encode([0], [morph_asset.DELTA_MIN, morph_asset.DELTA_MAX, 0])
        with self.assertRaises(ValueError):
            morph_asset.encode([0], [morph_asset.DELTA_MIN - 1, 0, 0])
        raw = bytearray(entry(deltas=(0, 0, 0)))
        raw[morph_asset.sizes(1, 1, False)[1]] = struct.pack("<b", morph_asset.DELTA_MIN - 1)[0]
        with self.assertRaises(ValueError):
            morph_asset.decode(bytes(raw))

    def test_a_shift_past_the_maximum_is_refused(self):
        morph_asset.encode([morph_asset.SHIFT_MAX], [0, 0, 0])
        with self.assertRaises(ValueError):
            morph_asset.encode([morph_asset.SHIFT_MAX + 1], [0, 0, 0])


def mesh_with(vertices, clusters, triangles=0):
    """The LMSH header bytes `validate_mesh` reads: vertex, triangle and cluster counts."""
    return mesh_asset.BLOB_HEADER.pack(vertices, triangles, clusters, 0, 1, 0, 0, 0, 0, 0, 0)


class MorphMeshBindingTests(unittest.TestCase):
    def test_counts_must_agree_with_the_mesh(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS)
        morph_asset.validate_mesh(blob, mesh_with(len(THREE_ROWS) // AXES, len(TWO_CLUSTER_SHIFTS)))
        for vertices, clusters in ((len(THREE_ROWS) // AXES + 1, len(TWO_CLUSTER_SHIFTS)),
                                   (len(THREE_ROWS) // AXES, len(TWO_CLUSTER_SHIFTS) + 1)):
            with self.subTest(vertices=vertices, clusters=clusters), self.assertRaisesRegex(ValueError, "disagree"):
                morph_asset.validate_mesh(blob, mesh_with(vertices, clusters))

    def test_a_landing_triangle_must_exist_in_the_mesh(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS, THREE_LANDING)
        last = max(triangle for triangle, _, _ in THREE_LANDING)
        counts = (len(THREE_ROWS) // AXES, len(TWO_CLUSTER_SHIFTS))
        morph_asset.validate_mesh(blob, mesh_with(*counts, last + 1))
        with self.assertRaisesRegex(ValueError, "triangle"):
            morph_asset.validate_mesh(blob, mesh_with(*counts, last))


class MorphEngineFrameTests(unittest.TestCase):
    def test_only_the_z_deltas_are_negated(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS, THREE_LANDING)
        mirrored = engine_frame.to_engine(morph_asset.TYPE, blob)
        shifts, deltas, landing = morph_asset.decode(mirrored)
        self.assertEqual(TWO_CLUSTER_SHIFTS, shifts)
        self.assertEqual(THREE_LANDING, landing)
        for index, (before, after) in enumerate(zip(THREE_ROWS, deltas)):
            self.assertEqual(-before if index % AXES == Z_AXIS else before, after, f"delta {index}")

    def test_mirroring_twice_returns_the_entry(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS)
        self.assertEqual(blob, engine_frame.to_engine(morph_asset.TYPE, engine_frame.to_engine(morph_asset.TYPE, blob)))

    def test_a_delta_at_the_limit_survives_the_mirror(self):
        blob = entry(deltas=(0, 0, morph_asset.DELTA_MAX))
        self.assertEqual([0, 0, morph_asset.DELTA_MIN],
                         morph_asset.decode(engine_frame.to_engine(morph_asset.TYPE, blob))[1])


def one_cluster(count):
    return [(0, count)]


ONE_PARENT = [0]


class MorphDeltaTests(unittest.TestCase):
    def test_a_move_that_fits_keeps_shift_zero_and_exact_deltas(self):
        pos = np.zeros((2, AXES), dtype=np.int64)
        target = np.array([[morph_asset.DELTA_MAX, 0, 0], [0, -morph_asset.DELTA_MAX, 5]])
        shifts, deltas = lit_mesh.morph_deltas(pos, target, one_cluster(2), ONE_PARENT)
        self.assertEqual([0], shifts)
        np.testing.assert_array_equal(target, deltas)

    def test_the_least_shift_that_fits_is_chosen(self):
        pos = np.zeros((1, AXES), dtype=np.int64)
        for shift in range(1, 5):
            for reach, expected in ((morph_asset.DELTA_MAX << shift, shift),
                                    ((morph_asset.DELTA_MAX << shift) + (1 << shift >> 1) - 1, shift),
                                    ((morph_asset.DELTA_MAX << shift) + (1 << shift >> 1), shift + 1)):
                with self.subTest(shift=shift, reach=reach):
                    shifts, deltas = lit_mesh.morph_deltas(pos, np.array([[reach, 0, 0]]), one_cluster(1), ONE_PARENT)
                    self.assertEqual([expected], shifts)
                    self.assertLessEqual(abs(int(deltas[0, 0])), morph_asset.DELTA_MAX)

    def test_each_move_rounds_within_half_a_step(self):
        rng = np.random.default_rng(1)
        pos = rng.integers(-1000, 1000, size=(40, AXES))
        move = rng.integers(-3000, 3000, size=(40, AXES))
        shifts, deltas = lit_mesh.morph_deltas(pos, pos + move, one_cluster(40), ONE_PARENT)
        step = 1 << shifts[0]
        self.assertGreater(shifts[0], 0)
        self.assertLessEqual(np.abs(move - deltas * step).max(), step / 2)
        self.assertLessEqual(np.abs(deltas).max(), morph_asset.DELTA_MAX)

    @staticmethod
    def small_and_large_move(parents):
        """Two one-vertex clusters under `parents`, one moving a tick and one four times DELTA_MAX."""
        pos = np.zeros((2, AXES), dtype=np.int64)
        target = np.array([[1, 0, 0], [morph_asset.DELTA_MAX * 4, 0, 0]])
        return lit_mesh.morph_deltas(pos, target, [(0, 1), (1, 1)], parents)

    def test_each_parent_group_picks_its_own_shift(self):
        shifts, deltas = self.small_and_large_move([0, 1])
        self.assertEqual([0, 2], shifts)
        self.assertEqual([1, morph_asset.DELTA_MAX], deltas[:, 0].tolist())

    def test_clusters_under_one_parent_share_the_shift_of_the_largest_move(self):
        shifts, deltas = self.small_and_large_move([0, 0])
        self.assertEqual([2, 2], shifts)
        self.assertEqual([0, morph_asset.DELTA_MAX], deltas[:, 0].tolist())

    def test_a_cluster_that_does_not_move_has_shift_zero(self):
        pos = np.arange(6).reshape(2, AXES)
        shifts, deltas = lit_mesh.morph_deltas(pos, pos, one_cluster(2), ONE_PARENT)
        self.assertEqual([0], shifts)
        self.assertFalse(deltas.any())


class CoarseTargetTests(unittest.TestCase):
    """Two coarse triangles of a square in z = 0, SIZE ticks a side, numbered FIRST and FIRST + 1."""

    SIZE = 10.0
    FIRST = 7
    CORNERS = np.array([[[0, 0, 0], [SIZE, 0, 0], [0, SIZE, 0]], [[SIZE, 0, 0], [SIZE, SIZE, 0], [0, SIZE, 0]]])
    UP = np.array([[0.0, 0.0, 1.0]])
    DOWN = -UP
    REACH = 1.0

    def coarse(self):
        return morph.Coarse(self.CORNERS, [self.FIRST, self.FIRST + 1])

    def test_a_row_on_a_coarse_corner_lands_on_that_corner_of_its_triangle(self):
        position, triangle, bary = self.coarse().land(np.array([[10.0, 10.0, 0.0]]), self.UP, self.REACH)
        np.testing.assert_array_equal([[10, 10, 0]], position)
        np.testing.assert_array_equal([self.FIRST + 1], triangle)
        np.testing.assert_allclose([[0, 1, 0]], bary, atol=1e-9)

    def test_a_row_off_the_surface_lands_on_it_with_the_weights_there(self):
        position, triangle, bary = self.coarse().land(np.array([[2.0, 3.0, 4.0]]), self.UP, self.REACH)
        np.testing.assert_array_equal([[2, 3, 0]], position)
        np.testing.assert_array_equal([self.FIRST], triangle)
        np.testing.assert_allclose([[0.5, 0.2, 0.3]], bary, atol=1e-9)

    def test_a_row_outside_the_surface_lands_on_its_nearest_edge(self):
        position, triangle, _ = self.coarse().land(np.array([[-5.0, 4.0, 2.0]]), self.UP, self.REACH)
        np.testing.assert_array_equal([[0, 4, 0]], position)
        np.testing.assert_array_equal([self.FIRST], triangle)

    def test_every_row_gets_its_own_landing(self):
        points = np.array([[2.0, 3.0, 4.0], [9.0, 9.0, 1.0]])
        position, triangle, bary = self.coarse().land(points, np.repeat(self.UP, 2, axis=0), self.REACH)
        np.testing.assert_array_equal([[2, 3, 0], [9, 9, 0]], position)
        np.testing.assert_array_equal([self.FIRST, self.FIRST + 1], triangle)
        self.assertEqual((2, 3), bary.shape)

    def layers(self):
        sheets = np.array([[[0, 0, 0], [10, 0, 0], [0, 10, 0]], [[0, 0, 1], [0, 10, 1], [10, 0, 1]]], dtype=float)
        return morph.Coarse(sheets, [self.FIRST, self.FIRST + 1])

    def test_a_row_prefers_a_coarse_triangle_facing_its_way_within_reach(self):
        # Nearer the sheet at z = 0, which faces up; the sheet at z = 1 faces down and is 0.2 ticks further.
        point = np.array([[2.0, 2.0, 0.4]])
        up = self.layers().land(point, self.UP, self.REACH)
        down = self.layers().land(point, self.DOWN, self.REACH)
        self.assertEqual((0, self.FIRST), (up[0][0, 2], up[1][0]))
        self.assertEqual((1, self.FIRST + 1), (down[0][0, 2], down[1][0]))

    def test_the_nearest_triangle_wins_when_the_facing_one_is_beyond_reach(self):
        point = np.array([[2.0, 2.0, 0.4]])
        position, triangle, _ = self.layers().land(point, self.DOWN, 0.1)
        self.assertEqual((0, self.FIRST), (position[0, 2], triangle[0]))


class KeptCornerTests(unittest.TestCase):
    REACH = 3.0

    def test_a_point_and_a_coarse_vertex_that_are_each_others_nearest_within_reach_are_one_vertex(self):
        points = np.array([[0.0, 0.0, 0.0], [100.0, 0.0, 0.0]])
        corners = np.array([[0.0, 0.0, 2.0], [100.0, 0.0, 0.0]])
        np.testing.assert_array_equal([0, 1], kept_corners(points, corners, self.REACH))

    def test_a_vertex_past_reach_is_not_kept(self):
        points = np.array([[0.0, 0.0, 0.0]])
        corners = np.array([[0.0, 0.0, self.REACH + 1]])
        np.testing.assert_array_equal([-1], kept_corners(points, corners, self.REACH))

    def test_a_vertex_nearer_another_point_is_not_kept_by_this_one(self):
        points = np.array([[0.0, 0.0, 0.0], [0.0, 0.0, 2.0]])
        corners = np.array([[0.0, 0.0, 2.0]])
        np.testing.assert_array_equal([-1, 0], kept_corners(points, corners, self.REACH))

    def test_of_two_corners_near_one_point_the_nearer_claims_it_and_the_other_takes_its_next_point(self):
        points = np.array([[0.0, 0.0, 0.0], [0.0, 0.0, 2.5]])
        near, far = [0.0, 0.0, 0.5], [0.0, 0.0, 1.0]
        np.testing.assert_array_equal([0, 1], kept_corners(points, np.array([near, far]), self.REACH))
        np.testing.assert_array_equal([1, 0], kept_corners(points, np.array([far, near]), self.REACH))

    def test_a_corner_with_no_unclaimed_point_within_reach_keeps_nothing(self):
        points = np.array([[0.0, 0.0, 0.0]])
        corners = np.array([[0.0, 0.0, 1.0], [0.0, 0.0, 0.5]])
        np.testing.assert_array_equal([1], kept_corners(points, corners, self.REACH))

    def test_the_slack_past_the_error_is_positive(self):
        self.assertGreater(KEPT_SLACK_TICKS, 0)


class LandingWeightTests(unittest.TestCase):
    def test_two_weights_out_of_the_whole_and_the_third_is_the_rest(self):
        bary = np.array([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0], [0.5, 0.25, 0.25]])
        first, second = landing_weights(bary)
        self.assertEqual([WHOLE, 0, 0, round(WHOLE * 0.5)], first.tolist())
        self.assertEqual([0, WHOLE, 0, round(WHOLE * 0.25)], second.tolist())

    def test_rounding_never_pushes_the_pair_past_the_whole(self):
        bary = np.array([[0.5, 0.5, 0.0], [1 / 3, 1 / 3, 1 / 3]])
        first, second = landing_weights(bary)
        self.assertLessEqual((first + second).max(), WHOLE)


class PushedVertexTests(unittest.TestCase):
    """A 3 x 3 grid of fine vertices, its centre kept by a coarse pyramid that
    raises the apex PUSH ticks (no more than the group's error)."""

    SPACING = 10
    PUSH = 3
    GRID = 3
    CENTRE = GRID * 1 + 1

    def mesh(self):
        grid = [(self.SPACING * i, self.SPACING * j, 0) for j in range(self.GRID) for i in range(self.GRID)]
        far = self.SPACING * (self.GRID - 1)
        coarse = [(self.SPACING, self.SPACING, self.PUSH), (0, 0, 0), (far, 0, 0), (0, far, 0), (far, far, 0)]
        fine = []
        for j in range(self.GRID - 1):
            for i in range(self.GRID - 1):
                a = self.GRID * j + i
                fine += [(a, a + 1, a + self.GRID + 1), (a, a + self.GRID + 1, a + self.GRID)]
        first = len(grid)
        pyramid = [(first, first + 1, first + 2), (first, first + 2, first + 4), (first, first + 4, first + 3),
                   (first, first + 3, first + 1)]
        box = np.zeros(3)
        return SimpleNamespace(
            pos=np.array(grid + coarse), rgb=None, face_colors=None, tris=np.array(fine + pyramid),
            clusters=[(0, first, 0, len(fine), box, box, False), (first, len(coarse), len(fine), len(pyramid), box, box, False)],
            lod=[(LOD_NONE, 0), (0, LOD_NONE)], groups=np.array([[self.SPACING, self.SPACING, 0, 20, self.PUSH]]))

    def test_a_kept_interior_vertex_lands_on_its_pushed_coarse_vertex(self):
        mesh = self.mesh()
        target, landing = read_targets(mesh)
        np.testing.assert_array_equal([self.SPACING, self.SPACING, self.PUSH], target[self.CENTRE])
        self.assertEqual(len(mesh.pos), len(landing))

    def test_the_kept_vertex_lands_on_a_coarse_triangle_with_its_whole_weight_on_the_apex(self):
        mesh = self.mesh()
        _, landing = read_targets(mesh)
        triangle, first, second = (int(v) for v in landing[self.CENTRE])
        self.assertIn(triangle, range(len(mesh.tris) - 4, len(mesh.tris)))
        apex = len(mesh.pos) - 5
        corner = list(mesh.tris[triangle]).index(apex)
        self.assertEqual(WHOLE, [first, second, WHOLE - first - second][corner])

    def test_a_face_coloured_mesh_has_no_landing(self):
        mesh = self.mesh()
        mesh.face_colors = np.zeros((len(mesh.tris), 3), dtype=int)
        target, landing = read_targets(mesh)
        self.assertIsNone(landing)
        np.testing.assert_array_equal([self.SPACING, self.SPACING, self.PUSH], target[self.CENTRE])


def parent_of(mesh):
    return [parent for _, parent in mesh.lod]


def coarse_corners(mesh, group):
    """The triangles of the clusters simplified from `group`, as corner positions."""
    out = []
    for (first, count, tri_first, tri_count, *_), (own, _) in zip(mesh.clusters, mesh.lod):
        if own == group:
            out.append(mesh.pos[mesh.tris[tri_first:tri_first + tri_count]])
    return np.concatenate(out).astype(np.float64)


def coarse_triangle_numbers(mesh, group):
    """The mesh triangle numbers of the clusters simplified from `group`."""
    out = []
    for (first, count, tri_first, tri_count, *_), (own, _) in zip(mesh.clusters, mesh.lod):
        if own == group:
            out.extend(range(tri_first, tri_first + tri_count))
    return out


def distance_to_triangles(point, corners):
    rows = np.repeat(point[None].astype(np.float64), len(corners), axis=0)
    _, distance = closest_point_on_triangles(rows, corners[:, 0], corners[:, 1], corners[:, 2])
    return float(distance.min())


class AboveTests(unittest.TestCase):
    def test_a_group_is_above_itself_and_every_ancestor_through_its_clusters_parents(self):
        mesh = SimpleNamespace(groups=[None] * 4, lod=[(1, 0), (2, 1), (LOD_NONE, 2), (3, LOD_NONE)])
        self.assertEqual({0: {0}, 1: {1, 0}, 2: {2, 1, 0}, 3: {3}}, morph.above(mesh))


class SharedRowTests(unittest.TestCase):
    """Two clusters of two rows, rows 0 and 2 at one position."""

    def mesh(self, lod):
        pos = np.array([[0, 0, 0], [5, 5, 5], [0, 0, 0], [9, 9, 9]])
        return SimpleNamespace(pos=pos, lod=lod, groups=[None] * 2, clusters=[(0, 2), (2, 2)])

    def test_clusters_under_different_parents_holding_a_position_hold_it(self):
        held = morph.shared_rows(self.mesh([(LOD_NONE, 0), (LOD_NONE, 1)]))
        self.assertEqual([True, False, True, False], held.tolist())

    def test_clusters_under_one_parent_hold_nothing(self):
        held = morph.shared_rows(self.mesh([(LOD_NONE, 0), (LOD_NONE, 0)]))
        self.assertFalse(held.any())

    def test_a_fine_cluster_and_the_coarse_one_simplified_from_its_parent_never_meet(self):
        held = morph.shared_rows(self.mesh([(LOD_NONE, 0), (0, LOD_NONE)]))
        self.assertFalse(held.any())


class BakedMorphTargetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        points, rgb, triangles, double = wavy_grid()
        baked = bake_lit_mesh(points, rgb, triangles, double, lod=True)
        cls.blob, cls.levels_blob = lit_mesh.mesh_blob(baked), lit_mesh.lod_blob(baked)
        cls.mesh = lit_mesh.read_lit_mesh(cls.blob, cls.levels_blob)
        cls.target, cls.landing = read_targets(cls.mesh)
        cls.plain = lit_mesh.bake_lit_mesh(points, rgb, triangles, double)

    def cluster_rows(self, c):
        first, count = self.mesh.clusters[c][:2]
        return np.arange(first, first + count)

    def test_a_mesh_with_levels_has_a_target_and_landing_for_every_row(self):
        self.assertEqual(self.mesh.pos.shape, self.target.shape)
        self.assertEqual((len(self.mesh.pos), AXES), self.landing.shape)

    def test_the_bundle_carries_a_morph_entry_only_when_asked_and_with_levels(self):
        kinds = [kind for _, kind, _ in mesh_bundle.mesh_entries("m", self.plain, morph=morph.morph_blob)]
        self.assertNotIn(morph_asset.TYPE, kinds)
        points, rgb, triangles, double = wavy_grid()
        levelled = bake_lit_mesh(points, rgb, triangles, double, lod=True)
        unasked = [kind for _, kind, _ in mesh_bundle.mesh_entries("m", levelled)]
        self.assertNotIn(morph_asset.TYPE, unasked)
        entries = mesh_bundle.mesh_entries("m", levelled, morph=morph.morph_blob)
        self.assertIn(("m.morph", morph_asset.TYPE), [(name, kind) for name, kind, _ in entries])

    def test_the_bundle_boxes_hold_every_target_so_a_morphing_cluster_is_never_culled(self):
        points, rgb, triangles, double = wavy_grid()
        levelled = bake_lit_mesh(points, rgb, triangles, double, lod=True)
        entries = {name: blob for name, _, blob in mesh_bundle.mesh_entries("m", levelled, morph=morph.morph_blob)}
        mesh = lit_mesh.read_lit_mesh(entries["m"], entries["m.lod"])
        target = lit_mesh.morph_targets(mesh.pos, mesh.clusters, entries["m.morph"])
        self.assertTrue(np.any(target != mesh.pos), "the fixture's targets move nothing")
        for first, count, _, _, lo, hi, _ in mesh.clusters:
            rows = target[first:first + count]
            self.assertTrue(np.all(rows >= lo) and np.all(rows <= hi))
        for node in mesh.nodes:
            held = mesh.clusters[node["first"]:node["first"] + node["count"]] if node["leaf"] else                 mesh.nodes[node["first"]:node["first"] + node["count"]]
            for part in held:
                part_lo, part_hi = (part[4], part[5]) if node["leaf"] else (part["lo"], part["hi"])
                self.assertTrue(np.all(part_lo >= node["lo"]) and np.all(part_hi <= node["hi"]))

    def test_every_row_with_a_parent_targets_a_point_on_the_parents_coarse_triangles(self):
        held = morph.shared_rows(self.mesh)
        self.assertTrue(held.any())
        checked = 0
        for c, (_, parent) in enumerate(self.mesh.lod):
            if parent == LOD_NONE:
                continue
            corners = coarse_corners(self.mesh, parent)
            for row in self.cluster_rows(c):
                if held[row]:
                    np.testing.assert_array_equal(self.mesh.pos[row], self.target[row])
                    continue
                self.assertLessEqual(distance_to_triangles(self.target[row], corners), ROUNDING_TICKS,
                                     f"cluster {c} row {row} is off the surface")
                checked += 1
        self.assertGreater(checked, 0)

    def test_every_landing_is_one_of_the_parents_coarse_triangles_with_valid_weights(self):
        checked = 0
        for c, (_, parent) in enumerate(self.mesh.lod):
            if parent == LOD_NONE:
                continue
            numbers = set(coarse_triangle_numbers(self.mesh, parent))
            for row in self.cluster_rows(c):
                triangle, first, second = (int(v) for v in self.landing[row])
                self.assertIn(triangle, numbers, f"cluster {c} row {row}")
                self.assertGreaterEqual(min(first, second), 0)
                self.assertLessEqual(first + second, WHOLE)
                checked += 1
        self.assertGreater(checked, 0)

    def test_a_cluster_without_a_parent_targets_itself(self):
        top = [c for c, (_, parent) in enumerate(self.mesh.lod) if parent == LOD_NONE]
        self.assertTrue(top)
        for c in top:
            rows = self.cluster_rows(c)
            np.testing.assert_array_equal(self.mesh.pos[rows], self.target[rows])

    def test_a_row_the_coarse_level_draws_at_its_own_position_targets_exactly_that(self):
        same = checked = 0
        for c, (_, parent) in enumerate(self.mesh.lod):
            if parent == LOD_NONE:
                continue
            corners = {tuple(p) for p in coarse_corners(self.mesh, parent).reshape(-1, 3).astype(np.int64)}
            for row in self.cluster_rows(c):
                if tuple(self.mesh.pos[row]) in corners:
                    checked += 1
                    same += int((self.target[row] == self.mesh.pos[row]).all())
        self.assertGreater(checked, 0)
        self.assertEqual(checked, same)

    def cluster_levels(self):
        """Each cluster's level: 0 for a finest one, else one above its own group's source clusters."""
        level = {c: 0 for c, (own, _) in enumerate(self.mesh.lod) if own == LOD_NONE}
        pending = [c for c in range(len(self.mesh.lod)) if c not in level]
        while pending:
            for c in list(pending):
                own = self.mesh.lod[c][0]
                sources = [level.get(k) for k, (_, parent) in enumerate(self.mesh.lod) if parent == own]
                if None not in sources:
                    level[c] = 1 + max(sources)
                    pending.remove(c)
        return level

    def cluster_triangles(self, c):
        first, tri_count = self.mesh.clusters[c][2:4]
        return self.mesh.tris[first:first + tri_count]

    def test_some_interior_row_moves_so_the_targets_are_not_all_trivial(self):
        moved = sum(int((self.target[c_rows] != self.mesh.pos[c_rows]).any(axis=1).sum())
                    for c_rows in (self.cluster_rows(c) for c in range(len(self.mesh.clusters))))
        self.assertGreater(moved, 0)

    def test_rows_at_one_position_under_one_parent_decode_to_one_target_whatever_their_clusters(self):
        shifts, deltas, _ = morph_asset.decode(morph.morph_blob(self.blob, self.levels_blob))
        deltas = np.array(deltas).reshape(-1, AXES)
        decoded = {}
        shared = 0
        for c, (first, count, *_) in enumerate(self.mesh.clusters):
            for row in range(first, first + count):
                at = (self.mesh.lod[c][1], *self.mesh.pos[row])
                landed = tuple(self.mesh.pos[row] + deltas[row] * (1 << shifts[c]))
                if at in decoded and decoded[at][0] != c:
                    shared += 1
                first_landed = decoded.setdefault(at, (c, landed))[1]
                self.assertEqual(first_landed, landed, f"cluster {c} row {row} at {at}")
        self.assertGreater(shared, 0)

    def test_no_two_clusters_drawable_together_leave_a_gap_at_a_shared_position(self):
        shifts, deltas, _ = morph_asset.decode(morph.morph_blob(self.blob, self.levels_blob))
        deltas = np.array(deltas).reshape(-1, AXES)
        ancestors = morph.above(self.mesh)
        at = {}
        for c, (first, count, *_) in enumerate(self.mesh.clusters):
            for row in range(first, first + count):
                at.setdefault(tuple(self.mesh.pos[row]), []).append((c, row))

        def apart(a, b):
            (own_a, parent_a), (own_b, parent_b) = self.mesh.lod[a], self.mesh.lod[b]
            return ((own_b != LOD_NONE and parent_a != LOD_NONE and own_b in ancestors[parent_a])
                    or (own_a != LOD_NONE and parent_b != LOD_NONE and own_a in ancestors[parent_b]))

        different = same = 0
        for place, rows in at.items():
            for i, (ca, ra) in enumerate(rows):
                for cb, rb in rows[i + 1:]:
                    if ca != cb and apart(ca, cb):
                        continue
                    move_a, move_b = deltas[ra] << shifts[ca], deltas[rb] << shifts[cb]
                    if self.mesh.lod[ca][1] == self.mesh.lod[cb][1]:
                        same += 1
                        np.testing.assert_array_equal(move_a, move_b, f"{place}: clusters {ca}, {cb}")
                    else:
                        different += 1
                        self.assertFalse(move_a.any() or move_b.any(), f"{place}: clusters {ca}, {cb} crack")
        self.assertGreater(different, 0)
        self.assertGreater(same, 0)

    def test_clusters_under_one_parent_share_a_shift_in_the_blob(self):
        shifts, _, _ = morph_asset.decode(morph.morph_blob(self.blob, self.levels_blob))
        by_parent = {}
        for shift, (_, parent) in zip(shifts, self.mesh.lod):
            by_parent.setdefault(parent, set()).add(shift)
        self.assertTrue(all(len(found) == 1 for found in by_parent.values()))

    def test_the_blob_round_trips_the_measured_targets_within_a_step(self):
        blob = morph.morph_blob(self.blob, self.levels_blob)
        shifts, deltas, landing = morph_asset.decode(blob)
        morph_asset.validate_mesh(blob, self.blob)
        self.assertEqual(len(self.mesh.clusters), len(shifts))
        deltas = np.array(deltas).reshape(-1, AXES)
        for c, (first, count, *_) in enumerate(self.mesh.clusters):
            rows = slice(first, first + count)
            decoded = self.mesh.pos[rows] + deltas[rows] * (1 << shifts[c])
            self.assertLessEqual(np.abs(decoded - self.target[rows]).max(), (1 << shifts[c]) / 2)
        self.assertEqual([tuple(int(v) for v in row) for row in self.landing], landing)


if __name__ == "__main__":
    unittest.main()
