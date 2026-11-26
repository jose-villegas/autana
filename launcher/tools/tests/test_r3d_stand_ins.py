"""Synthetic carved shapes for candidate choice, seam locks and isolated-view scoring."""

import pathlib
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np
from PIL import Image
from scipy.spatial import ConvexHull

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import gltf_preview, ray_query
from r3d.meshopt import PERMISSIVE, REGULARIZE_LIGHT, simplify_with_update
from r3d.objects import stand_ins
from r3d.simplify import _shared_passes
from tests.soup import box, concatenate
from tests.r3d_env import needs_llvm  # noqa: E402
from tests.test_r3d_bake import grid

BUDGET = 24
SHEET_QUADS = 24
SHEET_SIZE = 100.0
SHEET_WAVE_HEIGHT = 5.0
SHEET_WAVES = 4
HEM_WIDTH_FRACTION = 1 / 8
HEM_HEIGHT = 3.0
HEM_STRIPES = 12
OUTLINE_BUDGET = 32
OUTLINE_PIXELS = 128
OUTLINE_FRAME_FILL = 0.9
# Minimum overlap of the original and simplified silhouettes from the front.
OUTLINE_IOU_MIN = 0.95


def table():
    # Planks as thick as a real table's: thickness 0.17 of the width, legs square.
    boxes = [(np.array([i * 0.121, 0., 1.]), np.array([0.12, 0.8, 0.02])) for i in range(12)]
    boxes += [(np.array([x, y, 0.]), np.array([0.05, 0.05, 1.]))
              for x in (0.025, 1.376) for y in (0.025, 0.725)]
    parts = []
    for start, size in boxes:
        part = box(size)
        part.apply_translation(start + size / 2)
        parts.append(part)
    mesh = concatenate(parts)
    return mesh.vertices, np.full_like(mesh.vertices, 180.), mesh.faces, np.zeros(len(mesh.faces), dtype=int)


def choose(data, budget=BUDGET, lock=None):
    return stand_ins.best_stand_in(*data, budget, PERMISSIVE, lock)


class StandInTests(unittest.TestCase):
    def test_coloured_hem_keeps_the_front_outline(self):
        positions, tris = grid(SHEET_QUADS)
        u, v = positions[:, :2].T / SHEET_QUADS
        hem = np.clip(1 - v / HEM_WIDTH_FRACTION, 0, 1)
        positions[:, :2] = (positions[:, :2] / SHEET_QUADS - 0.5) * SHEET_SIZE
        positions[:, 2] = SHEET_WAVE_HEIGHT * np.sin(2 * np.pi * SHEET_WAVES * u) + HEM_HEIGHT * hem**2
        grey = stand_ins.RGB_MAX / 2 * (1 + hem * np.cos(2 * np.pi * HEM_STRIPES * u))
        rgb = np.broadcast_to(grey[:, None], positions.shape)
        materials = np.zeros(len(tris), dtype=int)
        result, _ = stand_ins.best_stand_in(positions, rgb, tris, materials, OUTLINE_BUDGET,
                                            REGULARIZE_LIGHT | PERMISSIVE, None)
        self.assertLessEqual(len(result.tris), OUTLINE_BUDGET)

        frame = OUTLINE_PIXELS / 2
        scale = OUTLINE_FRAME_FILL * OUTLINE_PIXELS / SHEET_SIZE
        camera = SimpleNamespace(project=lambda p: (frame + scale * p[0], frame - scale * p[1], -p[2]))
        masks = []
        for points, faces in ((positions, tris), (result.positions, result.tris)):
            image = Image.new('1', (OUTLINE_PIXELS, OUTLINE_PIXELS))
            gltf_preview.rasterize(image, camera, points, faces, lambda *_: (1, 1, 1))
            masks.append(np.asarray(image))
        source, simplified = masks
        iou = np.count_nonzero(source & simplified) / np.count_nonzero(source | simplified)
        self.assertGreaterEqual(iou, OUTLINE_IOU_MIN, f'front outline IoU {iou:.6f}')

    def test_plank_table(self):
        with patch.object(ray_query, 'VARIANT', 'llvm_ad_rgb'):
            candidate, rows = choose(table())
        assert candidate.kind == 'plane'
        assert rows[1]['error'] < rows[0]['error']
        assert len(candidate.tris) <= BUDGET
        top = candidate.positions[candidate.positions[:, 2] > 1.]
        assert np.ptp(top[:, 0]) >= 1.451 - 1e-6
        assert np.ptp(top[:, 1]) >= 0.8


    def test_organic_regular_is_identical_and_unscored(self):
        from itertools import product

        points = np.array(list(product((-1., 0., 1.), repeat=3)))
        points = points[np.any(points, axis=1)]
        points /= np.linalg.norm(points, axis=1, keepdims=True)
        tris = ConvexHull(points).simplices
        rgb, materials = np.full_like(points, 180.), np.zeros(len(tris), dtype=int)
        budget = 40
        expected = simplify_with_update(points, rgb, tris, budget, 0.0, PERMISSIVE, vertex_lock=None)
        with patch.object(stand_ins, 'view_error', side_effect=AssertionError('unexpected scoring')):
            result, rows = choose((points, rgb, tris, materials), budget)
        assert result.kind == 'regular'
        for actual, wanted in zip((result.positions, result.rgb, result.tris, result.kept), expected):
            assert actual.tobytes() == wanted.tobytes()
        assert rows == [dict(kind='regular', triangles=len(result.tris), error=None, chosen=True)]


    def test_lock_disallows_plane(self):
        data = table()
        lock = np.zeros(len(data[0]), dtype=bool)
        lock[0] = True
        assert stand_ins.plane(*data, BUDGET, PERMISSIVE, lock) is None
        with patch.object(stand_ins, 'view_error', side_effect=AssertionError('unexpected scoring')):
            assert choose(data, lock=lock)[0].kind == 'regular'


    def test_rectangles_over_budget(self):
        assert stand_ins.plane(*table(), 2, PERMISSIVE, None) is None


    def test_plate_edge_can_join_without_nearby_corners(self):
        wide, narrow = box([1., 1., 0.01]), box([0.2, 1., 0.01])
        narrow.apply_translation([0., 1.001, 0.])
        mesh = concatenate([wide, narrow])
        candidate = stand_ins.plane(mesh.vertices, np.full_like(mesh.vertices, 180.), mesh.faces,
            np.zeros(len(mesh.faces), dtype=int), len(stand_ins.RECTANGLE_TRIS), PERMISSIVE, None)
        assert candidate is not None


    @needs_llvm
    def test_self_and_hole_error(self):
        p, c, t, m = table()
        source = stand_ins.StandIn('source', p, c, t, m, None)
        hole = stand_ins.StandIn('hole', p, c, t[12:], m[12:], None)
        with patch.object(ray_query, 'VARIANT', 'llvm_ad_rgb'):
            assert stand_ins.view_error(source, source) == 0.
            assert stand_ins.view_error(source, hole) > 0.


    def test_shared_copies(self):
        p, c, t, m = table()
        offset = np.array([4., 0., 0.])
        pos, rgb = np.concatenate([p, p + offset]), np.concatenate([c, c])
        tris, labels = np.concatenate([t, t + len(p)]), np.concatenate([m, m])
        owners = np.repeat([0, 1], len(t))
        rule = dict(triangles=BUDGET)
        shapes = []
        with patch.object(ray_query, 'VARIANT', 'llvm_ad_rgb'):
            emitted, consumed, rows = _shared_passes(pos, rgb, tris, labels, owners, {0: rule, 1: rule},
                {0: 'free', 1: 'free'}, [(owners == i, BUDGET) for i in range(2)],
                np.zeros(len(pos), dtype=bool), PERMISSIVE, None, shapes)
        assert not emitted
        assert consumed == {0, 1}
        assert rows[0]['stand_in'] == 'plane'
        assert rows[0]['stand_ins'][1]['chosen']
        copies = shapes[0]['copies']
        np.testing.assert_allclose(copies[1][1][0] - copies[0][1][0], np.broadcast_to(offset, copies[0][1][0].shape), atol=1e-6)
        assert len(copies[0][1][2]) <= BUDGET
