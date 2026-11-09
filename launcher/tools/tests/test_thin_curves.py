"""Thin surface detection, curve fitting and the LCRV encoding on built shapes."""

import pathlib
import sys
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from asset.asset_pack import NAME_BYTES, PackError
from r3d.thin.curve_format import CURVE, HEADER, KNOT, NAME_SUFFIX, VERSION
from r3d.thin.thin_parts import (THIN_LENGTH_PER_SIDE, THIN_SLENDERNESS_MIN, WELD_CELLS_PER_SIDE, ThinPart, budget_side,
                                 find_thin_parts, thin_parts_at)
from r3d.thin.curve_fit import CURVE_LIGHT_RING, CURVE_LIGHT_SPAN, TUBE_SIDES, Curve, fit_curve, light_curve
from r3d.thin.curve_asset import I16_MAX, name_for_mesh, read_curves, write_curves
from r3d.import_settings import SettingsError, load_geometry
from r3d.light import LIGHT_GAMMA
from tests import soup

SIDE = 53.5
SCALE = 64


def cylinder():
    angle = np.arange(96) * 2 * np.pi / 96
    p = np.array([[2 * np.cos(a), 2 * np.sin(a), z] for z in (0, 200) for a in angle])
    t = np.array([[i, (i + 1) % len(angle), i + len(angle)] for i in range(len(angle))] +
                 [[(i + 1) % len(angle), (i + 1) % len(angle) + len(angle), i + len(angle)] for i in range(len(angle))])
    return p, t


def sagging(half_width, depth, sides=12):
    """A tube of radius `half_width` (sides >= 3), or a flat band that wide (sides == 2), hanging 600 long with
    `depth` of sag."""
    x = np.linspace(0, 600, 61)
    z = -depth * (1 - ((x - 300) / 300) ** 2)
    angle = np.arange(sides) * 2 * np.pi / sides
    ring = [(np.cos(a), np.sin(a)) for a in angle] if sides > 2 else [(0, -1), (0, 1)]
    p = np.array([[xi, half_width * c, zi + half_width * s] for xi, zi in zip(x, z) for c, s in ring])
    n = len(ring)
    edges = range(n) if sides > 2 else range(1)
    t = np.array([[k * n + i, k * n + (i + 1) % n, (k + 1) * n + i] for k in range(len(x) - 1) for i in edges] +
                 [[k * n + (i + 1) % n, (k + 1) * n + (i + 1) % n, (k + 1) * n + i]
                  for k in range(len(x) - 1) for i in edges])
    return p, t


def grey_cylinder():
    """The cylinder's thin part, its fitted curve and a grey source to light it from."""
    p, t = cylinder()
    part = find_thin_parts(p, t, SIDE)[0]
    source = SimpleNamespace(p=p, tri_v=t, tri_t=t, tri_m=np.zeros(len(t), dtype=int),
                             uv=np.zeros((len(p), 2)), materials={'surface': {'Kd': (0.25, 0.25, 0.25)}},
                             names=['surface'], textures=[None], colors=None)
    return part, fit_curve(p, t, part), source


def beside(bar, wall_size, offset, *more):
    """`bar` with a wall `wall_size` (x, z) one thick, its centre `offset` away along y, and `more` shapes."""
    wall = soup.box((wall_size[0], 1, wall_size[1]))
    wall.vertices = wall.vertices + [0, offset, 0]
    return soup.concatenate([bar, wall, *more])


def line_curve(knots, radius, colour=0.5):
    """A curve through `knots`, `radius` thick everywhere, one grey."""
    knots = np.asarray(knots, dtype=float)
    return Curve(knots, np.full(len(knots), float(radius)), np.full(knots.shape, colour))


TICKS = 1  # ticks to a unit: the entries below are written in ticks
TICK_KNOTS = [(0, 0, 0, 1, 0), (10, 0, 0, 1, 0), (0, 5, 0, 1, 0), (10, 5, 0, 1, 0)]


def lcrv(ranges=((0, 2), (2, 2)), knots=TICK_KNOTS, version=VERSION, reserved=0, shrink_first_box=False, extra=b""):
    """An LCRV entry over `knots` of one tick a unit, the knots of each (first, count) in `ranges` a curve, its box
    just holding their tubes; `shrink_first_box` takes a tick off the first box's far x."""
    rows = []
    for index, (first, count) in enumerate(ranges):
        tube = np.array(knots[first:first + count])
        lo = (tube[:, :3] - tube[:, 3:4]).min(axis=0)
        hi = (tube[:, :3] + tube[:, 3:4]).max(axis=0)
        hi[0] -= shrink_first_box and index == 0
        rows.append(CURVE.pack(first, count, *lo.astype(int), *hi.astype(int)))
    return (HEADER.pack(len(ranges), len(knots), version, reserved) + b"".join(rows)
            + b"".join(KNOT.pack(*knot) for knot in knots) + extra)


def bake_curves(folder, source, geometry=''):
    """Bakes `source` with curves on and an eight-triangle start, the simplifier passing triangles through: its job
    and its geometry. `geometry` adds lines to the import's [geometry] table."""
    from r3d import mesh_import
    from r3d.import_settings import albedo_jobs, load_import_settings

    path = pathlib.Path(folder) / 'mesh.import.toml'
    path.write_text('[source]\npath = "mesh.obj"\ncredit = "test"\n[output]\n'
                    f'[geometry]\ncurves = true\n{geometry}'
                    'simplify = { dense_edge = 1000, props = [], props_share = 0.3, '
                    'seal_seams = true, colour_deviation = 1.0 }\n'
                    '[[variants]]\nname = "mesh"\ntriangles = 8\n')
    job, = albedo_jobs(load_import_settings(path))
    with mock.patch.object(mesh_import, 'load_source', return_value=source), \
            mock.patch.object(mesh_import, 'simplify', side_effect=lambda p, c, t, m, *a, **k: (p, c, t, m)):
        return job, mesh_import.bake_geometry(job, None)


class Open:
    """An intersector with nothing in the way."""

    def first_hits(self, origins, directions):
        return np.zeros(len(origins), bool), np.zeros(len(origins)), np.zeros(len(origins), np.int64)


class Wall:
    """An intersector with a wall at x = `at`: rays heading +x hit it."""

    def __init__(self, at):
        self.at = at

    def first_hits(self, origins, directions):
        ahead = directions[:, 0] > 1e-9
        distance = np.where(ahead, (self.at - origins[:, 0]) / np.where(ahead, directions[:, 0], 1), 0)
        return ahead & (distance > 0), distance, np.zeros(len(origins), np.int64)


class ThinCurvesTests(unittest.TestCase):
    def test_thin_box(self):
        shape = soup.box((4.5, 4.5, 310))
        self.assertEqual(len(find_thin_parts(shape.vertices, shape.faces, SIDE)), 1)

    def test_triangle_sliver(self):
        p = np.array([[0, 0, 0], [3.6, 0, 0], [0, 0, 83]])
        self.assertEqual(len(find_thin_parts(p, np.array([[0, 1, 2]]), SIDE)), 1)

    def test_leaf_strip(self):
        p = np.array([[x, np.sin(z / 59), z] for z in np.linspace(0, 59, 10) for x in (0, 9)])
        t = np.array([[2*i, 2*i+1, 2*i+2] for i in range(9)] + [[2*i+1, 2*i+3, 2*i+2] for i in range(9)])
        self.assertEqual(find_thin_parts(p, t, SIDE), [])

    def test_band(self):
        shape = soup.box((36, 36, 330))
        self.assertEqual(find_thin_parts(shape.vertices, shape.faces, SIDE), [])

    def test_a_post_shorter_than_a_side_is_thin(self):
        length = 0.85 * SIDE
        shape = soup.box((length / 11.5, length / 11.5, length))
        self.assertEqual(len(find_thin_parts(shape.vertices, shape.faces, SIDE)), 1)

    def test_short_part(self):
        length = 0.9 * THIN_LENGTH_PER_SIDE * SIDE
        width = length / (THIN_SLENDERNESS_MIN + 1)
        shape = soup.box((width, width, length))
        self.assertEqual(find_thin_parts(shape.vertices, shape.faces, SIDE), [])

    def test_position_weld(self):
        shape = soup.box((4.5, 4.5, 155))
        p, t = shape.vertices, shape.faces
        p = np.concatenate([p, p + [0, 0, 155]])
        parts = find_thin_parts(p, np.concatenate([t, t + len(p)//2]), SIDE)
        self.assertEqual(len(parts), 1)
        self.assertEqual(len(parts[0].triangles), 2 * len(t))

    def test_bar_on_a_wall_rests(self):
        shape = beside(soup.box((4.5, 4.5, 310)), (400, 400), 2.75)
        self.assertEqual(find_thin_parts(shape.vertices, shape.faces, SIDE), [])

    def test_bar_on_a_wall_of_large_triangles_rests(self):
        specks = [soup.box((0.5, 0.5, 0.5)) for _ in range(20)]
        for k, speck in enumerate(specks):
            speck.vertices = speck.vertices + [30 + k, -30, 0]
        shape = beside(soup.box((4.5, 4.5, 310)), (4000, 4000), 2.75, *specks)
        self.assertEqual(find_thin_parts(shape.vertices, shape.faces, SIDE), [])

    def test_proximity_finds_what_a_full_search_finds(self):
        from r3d.geometry import TriangleProximity, closest_point_on_triangles
        rng = np.random.default_rng(3)
        corners = rng.random((300, 3, 3)) * 100
        corners[:5] *= 40
        proximity = TriangleProximity(corners)
        for point in rng.random((40, 3)) * 100:
            _, distance = closest_point_on_triangles(np.broadcast_to(point, (len(corners), 3)), *corners.transpose(1, 0, 2))
            np.testing.assert_array_equal(proximity.within(point, 6.0), np.flatnonzero(distance <= 6.0))

    def test_bars_flush_with_each_other_rest(self):
        left, right = soup.box((4.5, 4.5, 310)), soup.box((4.5, 4.5, 310))
        right.vertices = right.vertices + [4.6, 0, 0]
        shape = soup.concatenate([left, right])
        self.assertEqual(find_thin_parts(shape.vertices, shape.faces, SIDE), [])

    def test_pole_off_a_wall_stands(self):
        pole = soup.box((4.5, 4.5, 310))
        shape = beside(pole, (400, 400), 40)
        self.assertEqual([len(part.triangles) for part in find_thin_parts(shape.vertices, shape.faces, SIDE)],
                         [len(pole.faces)])

    def test_a_sagging_cable_is_as_thin_as_a_taut_one(self):
        p, t = sagging(2, 60)
        part, = find_thin_parts(p, t, SIDE)
        self.assertAlmostEqual(part.width, 4, delta=0.5)
        curve = fit_curve(p, t, part)
        self.assertAlmostEqual(curve.knots[:, 2].min(), -60, delta=3)
        np.testing.assert_allclose(curve.radius, 2, rtol=0.15)

    def test_a_sagging_band_wider_than_thin_stays(self):
        p, t = sagging(10, 60, sides=2)
        self.assertEqual(find_thin_parts(p, t, SIDE), [])

    def test_budget_area(self):
        p = np.array([[0, 0, 0], [4, 0, 0], [0, 2, 0]])
        self.assertEqual(budget_side(p, np.array([[0, 1, 2]]), 4), 1)

    def test_cylinder_fit(self):
        p, t = cylinder()
        for seed in (0, 1, 19):
            with self.subTest(seed=seed), mock.patch('r3d.thin.curve_fit.SAMPLE_SEED', seed):
                curve = fit_curve(p, t, find_thin_parts(p, t, SIDE)[0])
                self.assertEqual(len(curve.knots), 2)
                np.testing.assert_allclose(curve.radius, 2, rtol=0.05)
                np.testing.assert_allclose(sorted(curve.knots[:, 2]), [0, 200], atol=1e-10)

    def test_bend_fit(self):
        p, t = cylinder()
        ring = p[:96]
        p = np.concatenate([ring, ring + [0, 0, 100], ring + [100, 0, 100]])
        t = np.concatenate([t, t + 96])
        axes = np.array([[1, -1, 0], [0, 0, np.sqrt(2)], [1, 1, 0]]) / np.sqrt(2)
        part = ThinPart(np.arange(len(t)), p.mean(axis=0), axes, np.array([145, 4, 4]), 4)
        curve = fit_curve(p, t, part)
        self.assertGreater(len(curve.knots), 2)
        self.assertLess(np.linalg.norm(curve.knots - [0, 0, 100], axis=1).min(), 6)

    def test_asset_round_trip(self):
        curve = Curve(np.array([[1.23, -2.1, 3.5], [4.1, 5, 6.23]]), np.array([0.25, 0.7]),
                      np.array([[1, 0, 0], [0, 1, 0]], dtype=float))
        result, = read_curves(write_curves([curve], SCALE), SCALE)
        np.testing.assert_allclose(curve.knots, result.knots, atol=1/SCALE)
        np.testing.assert_allclose(curve.radius, result.radius, atol=1/SCALE)
        np.testing.assert_allclose(curve.color, result.color)

    def test_ring_lighting(self):
        part, curve, source = grey_cylinder()
        from r3d.light import light
        with mock.patch('r3d.light.light', wraps=light) as shade:
            lit = light_curve(curve, source, part, Open(),
                              [{'type': 'ambient', 'color': [1, 1, 1], 'intensity': 2}], 0.5)
        points, normals = shade.call_args.args[:2]
        self.assertEqual(len(points), len(curve.knots) * (1 + 2 * len(CURVE_LIGHT_SPAN)) * CURVE_LIGHT_RING)
        np.testing.assert_allclose(np.linalg.norm(normals, axis=1), 1)
        axis = (curve.knots[1] - curve.knots[0]) / np.linalg.norm(curve.knots[1] - curve.knots[0])
        np.testing.assert_allclose(normals @ axis, 0, atol=1e-9)
        offset = points - curve.knots[0]
        radial = np.linalg.norm(offset - np.outer(offset @ axis, axis), axis=1)
        self.assertTrue(np.all((radial >= curve.radius.min() - 1e-9) & (radial <= curve.radius.max() + 1e-9)))
        self.assertTrue(np.all((offset @ axis >= -1e-9) & (offset @ axis <= np.linalg.norm(curve.knots[1] - curve.knots[0]) + 1e-9)))
        np.testing.assert_allclose(lit.color, 2 * 0.25 ** LIGHT_GAMMA)
        np.testing.assert_array_equal(lit.knots, curve.knots)

    def test_a_knot_inside_a_wall_is_lit_along_the_stretch_it_draws(self):
        part, curve, source = grey_cylinder()

        def walled(points, *args, **kwargs):
            return np.where(points[:, 2:3] < 10, 0.0, 1.0) * np.ones((len(points), 3))

        with mock.patch('r3d.light.light', walled):
            lit = light_curve(curve, source, part, Open(), [], 0.5)
        end = int(np.argmin(curve.knots[:, 2]))
        self.assertGreater(lit.color[end].min(), 0.5 * lit.color[1 - end].min())

    def test_a_pole_beside_a_wall_takes_the_colour_of_its_open_side(self):
        part, curve, source = grey_cylinder()

        def shaded_toward_the_wall(points, normals, *args, **kwargs):
            return np.where(normals[:, 0:1] > 1e-9, 0.0, 1.0) * np.ones((len(points), 3))

        with mock.patch('r3d.light.light', shaded_toward_the_wall):
            beside = light_curve(curve, source, part, Wall(3.0), [], 0.0)
            far = light_curve(curve, source, part, Wall(1e6), [], 0.0)
        np.testing.assert_allclose(beside.color, 0.25 ** LIGHT_GAMMA)
        self.assertTrue(np.all(far.color < beside.color))

    def test_frozen_tubes(self):
        from r3d.fit.copies import placed_curves, joined
        curve = Curve(np.array([[0., 0, 0], [0., 0, 10]]), np.ones(2), np.ones((2, 3)))
        for flat in (False, True):
            frozen = placed_curves([curve], flat)
            self.assertEqual(len(frozen.tris), 2 * TUBE_SIDES + 2 * (TUBE_SIDES - 2))
            self.assertEqual(len(frozen.points), 2 * TUBE_SIDES)
            self.assertEqual(len(frozen.rgb), len(frozen.tris) if flat else len(frozen.points))
            before = frozen.points.copy()
            both = joined(frozen, frozen)
            self.assertEqual(len(both.tris), 2 * len(frozen.tris))
            np.testing.assert_array_equal(frozen.points, before)

    def test_start_excludes_curve_triangles(self):
        from r3d import mesh_import
        from r3d.thin import curve_asset
        from r3d import mesh_bundle, mesh_asset

        thin = soup.box((4.5, 4.5, 310))
        body = soup.box((50, 50, 50))
        p, t = thin.vertices, thin.faces
        rest, rest_t = body.vertices, body.faces
        rest += [125, 0, 0]
        source = SimpleNamespace(p=np.concatenate([p, rest]), tri_v=np.concatenate([t, rest_t + len(p)]),
                                 tri_m=np.zeros(len(t) + len(rest_t), dtype=int), names=['surface'],
                                 tri_o=np.zeros(len(t) + len(rest_t), dtype=int), object_names=['object'],
                                 materials={'surface': {'Kd': (1, 1, 1)}}, textures=[None], colors=None,
                                 uv=np.zeros((len(p) + len(rest), 2)), tri_t=np.concatenate([t, rest_t + len(p)]))
        with tempfile.TemporaryDirectory() as folder:
            job, geometry = bake_curves(folder, source)
            self.assertEqual(job.asset_path.suffix, mesh_bundle.SUFFIX)
            self.assertEqual(len(geometry.curves), 1)
            self.assertEqual(len(geometry.tris), len(rest_t))
            self.assertGreaterEqual(geometry.positions[:, 0].min(), 100)
            mesh_import.write_baked(job, None, folder, 'mesh', geometry)
            entries, copies = mesh_bundle.decode((pathlib.Path(folder) / 'mesh.bundle').read_bytes())
            self.assertEqual(copies, [])
            self.assertEqual({kind for _, kind, _ in entries}, {mesh_asset.TYPE, curve_asset.TYPE})
            from r3d.fit.copies import placed_copies
            frozen = placed_copies(pathlib.Path(folder) / 'mesh.bundle', False)
            self.assertGreater(len(frozen.tris), 0)
            self.assertLess(frozen.points[:, 0].max(), 100)
            source.p, source.tri_v, source.tri_t = p, t, t
            source.tri_m = source.tri_o = np.zeros(len(t), dtype=int)
            source.uv = np.zeros((len(p), 2))
            with mock.patch.object(mesh_import, 'load_source', return_value=source), \
                    self.assertRaisesRegex(ValueError, 'every shown triangle is thin'):
                mesh_import.bake_geometry(job, None)

    def test_a_thinned_material_is_never_searched_for_curves(self):
        from r3d import mesh_import
        from r3d.import_settings import thinned_triangles
        from r3d.thin.thin_parts import thin_triangles

        wire, body = soup.box((4.5, 4.5, 310)), soup.box((50, 50, 50))
        p, t = wire.vertices, wire.faces
        rest, rest_t = body.vertices + [125, 0, 0], body.faces
        names = ['wire', 'surface']
        tri_m = np.concatenate([np.zeros(len(t), dtype=int), np.ones(len(rest_t), dtype=int)])
        triangles = np.concatenate([t, rest_t + len(p)])
        source = SimpleNamespace(p=np.concatenate([p, rest]), tri_v=triangles, tri_t=triangles, tri_m=tri_m,
                                 names=names, tri_o=np.zeros(len(tri_m), dtype=int), object_names=['object'],
                                 materials={name: {'Kd': (1, 1, 1)} for name in names}, textures=[None] * len(names), colors=None,
                                 uv=np.zeros((len(p) + len(rest), 2)))
        for keep, triangles_out in ((0, len(rest_t)), (1, len(tri_m))):
            with self.subTest(keep=keep), tempfile.TemporaryDirectory() as folder:
                job, geometry = bake_curves(folder, source, f'thin = {{ material = "wire", keep = {keep} }}\n')
                self.assertEqual(len(geometry.curves), 0)
                self.assertEqual(len(geometry.tris), triangles_out)
                owned = thinned_triangles(job.settings.thin, tri_m, names)
                self.assertFalse(thin_triangles(source.p, triangles, job.renderer.variant.triangles, owned).any())
                self.assertTrue(thin_triangles(source.p, triangles, job.renderer.variant.triangles).any())

    def test_a_radius_that_exceeds_its_box_once_rounded_is_refused(self):
        # Ties round to even: 1.5 rounds up to 2 and the knot's tick to its even neighbour, so the rounded tube ends
        # a tick past the last one the box can name, though the unrounded one ends on it.
        edge = I16_MAX - 2
        write_curves([line_curve([[edge, 0, 0], [0, 0, 0]], 1.5)], TICKS)
        with self.assertRaisesRegex(PackError, "radius exceeds its box"):
            write_curves([line_curve([[edge + 0.5, 0, 0], [0, 0, 0]], 1.5)], TICKS)

    def test_a_negative_colour_is_refused(self):
        valid = line_curve([[0, 0, 0], [1, 0, 0]], 0.5)
        write_curves([valid], TICKS)
        negative = Curve(valid.knots, valid.radius, valid.color - [[0, 0, 0], [0, 0, 0.5 + 1e-3]])
        with self.assertRaisesRegex(PackError, "arrays are invalid"):
            write_curves([negative], TICKS)

    def test_a_curve_of_one_knot_is_refused(self):
        write_curves([line_curve([[0, 0, 0], [1, 0, 0]], 0.5)], TICKS)
        with self.assertRaisesRegex(PackError, "arrays are invalid"):
            write_curves([line_curve([[0, 0, 0]], 0.5)], TICKS)

    def test_a_mesh_id_with_no_room_for_its_curves_is_refused(self):
        room = NAME_BYTES - 1 - len(NAME_SUFFIX)
        self.assertEqual(name_for_mesh("m" * room), "m" * room + NAME_SUFFIX)
        with self.assertRaisesRegex(PackError, "no room"):
            name_for_mesh("m" * (room + 1))

    def test_a_valid_entry_reads_back(self):
        self.assertEqual(len(read_curves(lcrv(), TICKS)), 2)

    def test_reading_refuses_a_box_that_does_not_hold_its_knots(self):
        with self.assertRaisesRegex(PackError, "box does not contain"):
            read_curves(lcrv(shrink_first_box=True), TICKS)

    def test_reading_refuses_a_knot_range_that_does_not_follow_on(self):
        for name, ranges in (("a row of one knot", ((0, 1), (1, 3))),
                             ("a row that skips back", ((0, 2), (1, 3))),
                             ("a row past the last knot", ((0, 2), (2, 3)))):
            with self.subTest(name), self.assertRaisesRegex(PackError, "knot range"):
                read_curves(lcrv(ranges), TICKS)

    def test_reading_refuses_a_wrong_version_and_a_reserved_word(self):
        for name, entry in (("version", lcrv(version=VERSION + 1)), ("reserved", lcrv(reserved=1))):
            with self.subTest(name), self.assertRaisesRegex(PackError, "sections are invalid"):
                read_curves(entry, TICKS)

    def test_reading_refuses_an_entry_of_another_length_than_its_header_says(self):
        with self.assertRaisesRegex(PackError, "sections are invalid"):
            read_curves(lcrv(extra=bytes(KNOT.size)), TICKS)
        with self.assertRaisesRegex(PackError, "header"):
            read_curves(lcrv()[:HEADER.size - 1], TICKS)

    def test_reading_refuses_knots_no_curve_owns(self):
        with self.assertRaisesRegex(PackError, "unowned"):
            read_curves(lcrv(knots=TICK_KNOTS + [(0, 9, 0, 1, 0)]), TICKS)

    def test_thin_parts_agree_for_float32_and_float64_copies_of_the_same_positions(self):
        # Two bars end to end, the first's top ring a hair under a weld cell's midpoint and the second's bottom ring a
        # hair over it: float64 puts them in two cells and float32 rounds the hair away, so only positions rounded
        # first give both loads one welded bar.
        from r3d.geometry import triangle_areas
        cell = SIDE / WELD_CELLS_PER_SIDE
        join = (np.floor(200 / cell) + 0.5) * cell
        hair = 1e-9
        p, t = cylinder()
        ring = len(p) // 2
        below, above = p.copy(), p.copy()
        below[ring:, 2] = join - hair
        above[:ring, 2] = join + hair
        above[ring:, 2] = 2 * join
        p, t = np.concatenate([below, above]), np.concatenate([t, t + len(p)])
        budget = triangle_areas(p, t).sum() / SIDE ** 2
        wide, narrow = thin_parts_at(p, t, budget), thin_parts_at(p.astype(np.float32), t, budget)
        self.assertEqual([len(part.triangles) for part in narrow], [len(t)])
        self.assertEqual([len(part.triangles) for part in wide], [len(t)])
        np.testing.assert_array_equal(wide[0].triangles, narrow[0].triangles)

    def test_settings_default(self):
        steps = SimpleNamespace()
        load_geometry({}, steps)
        self.assertFalse(steps.curves)

    def test_settings_type(self):
        with self.assertRaises(SettingsError):
            load_geometry({'curves': 1}, SimpleNamespace())


if __name__ == '__main__':
    unittest.main()
