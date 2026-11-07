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
from r3d.lod.morph import KEPT_SLACK_TICKS, kept_corners, open_border, read_targets, shared_positions
from r3d.mesh_asset import LOD_NONE
from test_r3d_lod_bake import wavy_grid

AXES = morph_asset.AXES
# A float32-free bound: rounding a point on a triangle to ticks moves it half a tick per axis.
ROUNDING_TICKS = np.sqrt(AXES) / 2.0 + 1e-9
Z_AXIS = 2
TWO_CLUSTER_SHIFTS = [0, 3]
THREE_ROWS = [1, -2, 3, -4, 5, -6, 7, 8, -9]
THREE_COLOURS = [10, 20, 30, 40, 50, 60, 70, 80, 90]


def entry(shifts=(0,), deltas=(1, 2, 3), colours=None):
    return morph_asset.encode(list(shifts), list(deltas), colours)


def patched_header(blob, version=None, flags=None, clusters=None, vertices=None):
    _, f, c, v = morph_asset.HEADER.unpack_from(blob)
    head = morph_asset.HEADER.pack(morph_asset.VERSION if version is None else version,
                                   f if flags is None else flags, c if clusters is None else clusters,
                                   v if vertices is None else vertices)
    return head + blob[morph_asset.HEADER.size:]


class MorphCodecTests(unittest.TestCase):
    def test_round_trip_without_colours(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS)
        shifts, deltas, colours = morph_asset.decode(blob)
        self.assertEqual((TWO_CLUSTER_SHIFTS, THREE_ROWS, None), (shifts, deltas, colours))

    def test_round_trip_with_colours(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS, THREE_COLOURS)
        shifts, deltas, colours = morph_asset.decode(blob)
        self.assertEqual((TWO_CLUSTER_SHIFTS, THREE_ROWS, THREE_COLOURS), (shifts, deltas, colours))
        self.assertEqual(len(blob), len(entry(TWO_CLUSTER_SHIFTS, THREE_ROWS)) + len(THREE_COLOURS) + 3)

    def test_layout_constants_come_from_the_c_header(self):
        self.assertEqual(morph_asset.HEADER.size, read_define(morph_asset.OWNER, "LMOR_HEADER_SIZE"))
        self.assertEqual(morph_asset.name_for_mesh("room"), "room.morph")
        self.assertEqual(morph_asset.SHIFT_MAX, read_define(morph_asset.OWNER, "LMOR_SHIFT_MAX"))

    def test_decode_rejects_sizes_that_do_not_match_the_counts(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS, THREE_COLOURS)
        for bad in (blob[:morph_asset.HEADER.size - 1], blob[:-1], blob + bytes(morph_asset.ALIGN),
                    patched_header(blob, vertices=7), patched_header(blob, clusters=9)):
            with self.subTest(size=len(bad)), self.assertRaises(ValueError):
                morph_asset.decode(bad)

    def test_decode_rejects_unknown_flags_and_versions(self):
        blob = entry()
        for bad in (patched_header(blob, flags=morph_asset.COLOURS << 1),
                    patched_header(blob, flags=morph_asset.COLOURS | (morph_asset.COLOURS << 1)),
                    patched_header(blob, version=morph_asset.VERSION + 1)):
            with self.subTest(header=bad[:morph_asset.HEADER.size]), self.assertRaises(ValueError):
                morph_asset.decode(bad)

    def test_a_flag_for_colours_the_entry_lacks_is_a_size_mismatch(self):
        with self.assertRaises(ValueError):
            morph_asset.decode(patched_header(entry(), flags=morph_asset.COLOURS))

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


def mesh_with(vertices, clusters):
    """The LMSH header bytes `validate_mesh` reads: vertex and cluster counts."""
    return mesh_asset.BLOB_HEADER.pack(vertices, 0, clusters, 0, 1, 0, 0, 0, 0, 0, 0)


class MorphMeshBindingTests(unittest.TestCase):
    def test_counts_must_agree_with_the_mesh(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS)
        morph_asset.validate_mesh(blob, mesh_with(len(THREE_ROWS) // AXES, len(TWO_CLUSTER_SHIFTS)))
        for vertices, clusters in ((len(THREE_ROWS) // AXES + 1, len(TWO_CLUSTER_SHIFTS)),
                                   (len(THREE_ROWS) // AXES, len(TWO_CLUSTER_SHIFTS) + 1)):
            with self.subTest(vertices=vertices, clusters=clusters), self.assertRaisesRegex(ValueError, "disagree"):
                morph_asset.validate_mesh(blob, mesh_with(vertices, clusters))


class MorphEngineFrameTests(unittest.TestCase):
    def test_only_the_z_deltas_are_negated(self):
        blob = entry(TWO_CLUSTER_SHIFTS, THREE_ROWS, THREE_COLOURS)
        mirrored = engine_frame.to_engine(morph_asset.TYPE, blob)
        shifts, deltas, colours = morph_asset.decode(mirrored)
        self.assertEqual(TWO_CLUSTER_SHIFTS, shifts)
        self.assertEqual(THREE_COLOURS, colours)
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

    def test_each_parent_group_picks_its_own_shift(self):
        pos = np.zeros((2, AXES), dtype=np.int64)
        target = np.array([[1, 0, 0], [morph_asset.DELTA_MAX * 4, 0, 0]])
        shifts, deltas = lit_mesh.morph_deltas(pos, target, [(0, 1), (1, 1)], [0, 1])
        self.assertEqual([0, 2], shifts)
        self.assertEqual([1, morph_asset.DELTA_MAX], deltas[:, 0].tolist())

    def test_clusters_under_one_parent_share_the_shift_of_the_largest_move(self):
        pos = np.zeros((2, AXES), dtype=np.int64)
        target = np.array([[1, 0, 0], [morph_asset.DELTA_MAX * 4, 0, 0]])
        shifts, deltas = lit_mesh.morph_deltas(pos, target, [(0, 1), (1, 1)], [0, 0])
        self.assertEqual([2, 2], shifts)
        self.assertEqual([0, morph_asset.DELTA_MAX], deltas[:, 0].tolist())

    def test_a_cluster_that_does_not_move_has_shift_zero(self):
        pos = np.arange(6).reshape(2, AXES)
        shifts, deltas = lit_mesh.morph_deltas(pos, pos, one_cluster(2), ONE_PARENT)
        self.assertEqual([0], shifts)
        self.assertFalse(deltas.any())


class CoarseTargetTests(unittest.TestCase):
    """Two coarse triangles of a square in z = 0, SIZE ticks a side."""

    SIZE = 10.0
    CORNERS = np.array([[[0, 0, 0], [SIZE, 0, 0], [0, SIZE, 0]], [[SIZE, 0, 0], [SIZE, SIZE, 0], [0, SIZE, 0]]])
    COLOURS = np.array([[[0, 0, 0], [100, 0, 0], [0, 100, 0]], [[100, 0, 0], [100, 100, 0], [0, 100, 0]]])
    UP = np.array([[0.0, 0.0, 1.0]])
    DOWN = -UP

    def coarse(self, colours=True):
        return morph.Coarse(self.CORNERS, self.COLOURS if colours else None)

    def test_a_row_on_a_coarse_corner_targets_that_corner_and_its_colour(self):
        position, colour = self.coarse().targets(np.array([[10.0, 10.0, 0.0]]), self.UP)
        np.testing.assert_array_equal([[10, 10, 0]], position)
        np.testing.assert_array_equal([[100, 100, 0]], colour)

    def test_a_row_off_the_surface_lands_on_it_with_the_colour_there(self):
        position, colour = self.coarse().targets(np.array([[2.0, 3.0, 4.0]]), self.UP)
        np.testing.assert_array_equal([[2, 3, 0]], position)
        np.testing.assert_array_equal([[20, 30, 0]], colour)

    def test_a_row_outside_the_surface_lands_on_its_nearest_edge(self):
        position, _ = self.coarse(False).targets(np.array([[-5.0, 4.0, 2.0]]), self.UP)
        np.testing.assert_array_equal([[0, 4, 0]], position)

    def test_colours_are_absent_when_the_coarse_level_has_none(self):
        _, colour = self.coarse(False).targets(np.array([[2.0, 3.0, 4.0]]), self.UP)
        self.assertIsNone(colour)

    def test_a_row_prefers_a_coarse_triangle_facing_its_way(self):
        layers = np.array([[[0, 0, 0], [10, 0, 0], [0, 10, 0]], [[0, 0, 1], [0, 10, 1], [10, 0, 1]]], dtype=float)
        c = morph.Coarse(layers, None)
        # Nearer the sheet at z = 0, which faces up; a downward normal picks the sheet at z = 1 instead.
        point = np.array([[2.0, 2.0, 0.4]])
        self.assertEqual(0, c.targets(point, self.UP)[0][0, 2])
        self.assertEqual(1, c.targets(point, self.DOWN)[0][0, 2])


class KeptAndBorderTests(unittest.TestCase):
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

    def test_the_slack_past_the_error_is_positive(self):
        self.assertGreater(KEPT_SLACK_TICKS, 0)

    def test_only_points_on_an_edge_one_triangle_uses_are_on_the_open_border(self):
        quad = np.array([[0, 1, 2], [1, 3, 2]])
        np.testing.assert_array_equal([True] * 4, open_border(quad, 4))
        fan = np.array([[0, 1, 2], [0, 2, 3], [0, 3, 4], [0, 4, 1]])
        np.testing.assert_array_equal([False, True, True, True, True], open_border(fan, 5))


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
            pos=np.array(grid + coarse), rgb=None, tris=np.array(fine + pyramid),
            clusters=[(0, first, 0, len(fine), box, box, False), (first, len(coarse), len(fine), len(pyramid), box, box, False)],
            lod=[(LOD_NONE, 0), (0, LOD_NONE)], groups=np.array([[self.SPACING, self.SPACING, 0, 20, self.PUSH]]))

    def test_a_kept_interior_vertex_lands_on_its_pushed_coarse_vertex(self):
        mesh = self.mesh()
        target, _ = read_targets(mesh)
        np.testing.assert_array_equal([self.SPACING, self.SPACING, self.PUSH], target[self.CENTRE])

    def test_the_rows_on_the_fine_patch_border_stay_put_and_the_coarse_cluster_targets_itself(self):
        mesh = self.mesh()
        target, _ = read_targets(mesh)
        for row in range(len(mesh.pos)):
            if row != self.CENTRE:
                np.testing.assert_array_equal(mesh.pos[row], target[row], f"row {row}")


def parent_of(mesh):
    return [parent for _, parent in mesh.lod]


def coarse_corners(mesh, group):
    """The triangles of the clusters simplified from `group`, as corner positions."""
    out = []
    for (first, count, tri_first, tri_count, *_), (own, _) in zip(mesh.clusters, mesh.lod):
        if own == group:
            out.append(mesh.pos[mesh.tris[tri_first:tri_first + tri_count]])
    return np.concatenate(out).astype(np.float64)


def distance_to_triangles(point, corners):
    rows = np.repeat(point[None].astype(np.float64), len(corners), axis=0)
    _, distance = closest_point_on_triangles(rows, corners[:, 0], corners[:, 1], corners[:, 2])
    return float(distance.min())


class BakedMorphTargetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        points, rgb, triangles, double = wavy_grid()
        baked = bake_lit_mesh(points, rgb, triangles, double, lod=True)
        cls.blob, cls.levels_blob = lit_mesh.mesh_blob(baked), lit_mesh.lod_blob(baked)
        cls.mesh = lit_mesh.read_lit_mesh(cls.blob, cls.levels_blob)
        cls.target, cls.colour = read_targets(cls.mesh)
        cls.plain = lit_mesh.bake_lit_mesh(points, rgb, triangles, double)

    def cluster_rows(self, c):
        first, count = self.mesh.clusters[c][:2]
        return np.arange(first, first + count)

    def test_a_mesh_with_levels_has_a_target_and_colour_for_every_row(self):
        self.assertEqual(self.mesh.pos.shape, self.target.shape)
        self.assertEqual(self.mesh.rgb.shape, self.colour.shape)
        self.assertGreaterEqual(int(self.colour.min()), 0)
        self.assertLessEqual(int(self.colour.max()), 255)

    def test_the_bundle_carries_a_morph_entry_only_with_levels(self):
        kinds = [kind for _, kind, _ in mesh_bundle.mesh_entries("m", self.plain)]
        self.assertNotIn(morph_asset.TYPE, kinds)
        points, rgb, triangles, double = wavy_grid()
        levelled = bake_lit_mesh(points, rgb, triangles, double, lod=True)
        entries = mesh_bundle.mesh_entries("m", levelled)
        self.assertIn(("m.morph", morph_asset.TYPE), [(name, kind) for name, kind, _ in entries])

    def stay_put_positions(self, parent):
        """Positions that keep their place under `parent`: its fine patch's open border, and any shared one."""
        members = [c for c, (_, p) in enumerate(self.mesh.lod) if p == parent]
        tris = np.concatenate([self.cluster_triangles(c) for c in members])
        places, ids = np.unique(self.mesh.pos[tris.reshape(-1)], axis=0, return_inverse=True)
        edge = open_border(ids.reshape(-1, 3), len(places))
        held = {tuple(p) for p in places[edge]}
        return held | {tuple(p) for p in shared_positions(self.mesh, self.mesh.pos)}

    def test_every_row_with_a_parent_targets_the_coarse_triangles_unless_its_place_is_held(self):
        on_surface = held = 0
        for c, (_, parent) in enumerate(self.mesh.lod):
            if parent == LOD_NONE:
                continue
            corners = coarse_corners(self.mesh, parent)
            stay = self.stay_put_positions(parent)
            for row in self.cluster_rows(c):
                if distance_to_triangles(self.target[row], corners) <= ROUNDING_TICKS:
                    on_surface += 1
                    continue
                self.assertIn(tuple(self.mesh.pos[row]), stay, f"cluster {c} row {row} is off the surface")
                np.testing.assert_array_equal(self.mesh.pos[row], self.target[row])
                held += 1
        self.assertGreater(on_surface, 0)
        self.assertGreater(held, 0)

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

    def test_rows_on_the_open_border_of_a_groups_fine_triangles_stay_where_they_are(self):
        border_rows = moved_rows = 0
        for group in range(len(self.mesh.groups)):
            members = [c for c, (_, parent) in enumerate(self.mesh.lod) if parent == group]
            tris = np.concatenate([self.cluster_triangles(c) for c in members])
            _, ids = np.unique(self.mesh.pos[tris.reshape(-1)], axis=0, return_inverse=True)
            edge = open_border(ids.reshape(-1, 3), int(ids.max()) + 1)
            for row, vertex in zip(tris.reshape(-1), ids.reshape(-1)):
                same = bool((self.target[row] == self.mesh.pos[row]).all())
                if edge[vertex]:
                    self.assertTrue(same, f"group {group} border row {row} moved")
                    border_rows += 1
                else:
                    moved_rows += 0 if same else 1
        self.assertGreater(border_rows, 0)
        self.assertGreater(moved_rows, 0, "no interior row moves: the targets are all trivial")

    def test_rows_at_one_position_decode_to_one_target_whatever_their_clusters_and_parents(self):
        shifts, deltas, _ = morph_asset.decode(lit_mesh.morph_blob(self.blob, self.levels_blob))
        deltas = np.array(deltas).reshape(-1, AXES)
        decoded = {}
        shared = 0
        for c, (first, count, *_) in enumerate(self.mesh.clusters):
            for row in range(first, first + count):
                at = tuple(self.mesh.pos[row])
                landed = tuple(self.mesh.pos[row] + deltas[row] * (1 << shifts[c]))
                if at in decoded and decoded[at][0] != c:
                    shared += 1
                first_landed = decoded.setdefault(at, (c, landed))[1]
                self.assertEqual(first_landed, landed, f"cluster {c} row {row} at {at}")
        self.assertGreater(shared, 0)
        self.assertGreater(len({parent for _, parent in self.mesh.lod}), 1)

    def test_clusters_under_one_parent_share_a_shift_in_the_blob(self):
        shifts, _, _ = morph_asset.decode(lit_mesh.morph_blob(self.blob, self.levels_blob))
        by_parent = {}
        for shift, (_, parent) in zip(shifts, self.mesh.lod):
            by_parent.setdefault(parent, set()).add(shift)
        self.assertTrue(all(len(found) == 1 for found in by_parent.values()))

    def test_the_blob_round_trips_the_measured_targets_within_a_step(self):
        blob = lit_mesh.morph_blob(self.blob, self.levels_blob)
        shifts, deltas, colours = morph_asset.decode(blob)
        morph_asset.validate_mesh(blob, self.blob)
        self.assertEqual(len(self.mesh.clusters), len(shifts))
        deltas = np.array(deltas).reshape(-1, AXES)
        for c, (first, count, *_) in enumerate(self.mesh.clusters):
            rows = slice(first, first + count)
            decoded = self.mesh.pos[rows] + deltas[rows] * (1 << shifts[c])
            self.assertLessEqual(np.abs(decoded - self.target[rows]).max(), (1 << shifts[c]) / 2)
        np.testing.assert_array_equal(np.array(colours).reshape(-1, AXES), self.colour)


if __name__ == "__main__":
    unittest.main()
