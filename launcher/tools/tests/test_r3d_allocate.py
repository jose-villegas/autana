"""Shape error allocation for prototypes and whole source objects."""

import inspect
import pathlib
import sys
import unittest
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import simplify
from r3d.geometry import bounds_diagonal
from r3d.import_settings import SettingsError
from r3d.meshopt import ERROR_ABSOLUTE, PRUNE, simplify_to_error
from r3d.objects import allocate as owner, detail
from test_r3d_detail import sphere

SPHERE_RINGS = 8
SMALL_SCALE = 0.01
COPY_SHIFT = np.array([512., -1024., 256.])


class SharedErrorTests(unittest.TestCase):
    def setUp(self):
        self.positions, self.tris = sphere(rings=SPHERE_RINGS)
        self.large = owner.Piece("large", self.positions, self.tris, 3, False)
        self.small = owner.Piece("small", self.positions * SMALL_SCALE, self.tris, 1, True)
        self.maximum = owner.piece_error(self.large, np.inf)
        self.coarse = [owner.piece_count(piece, self.maximum) for piece in (self.large, self.small)]

    def test_drawn_and_stored_each_bind(self):
        large, small = self.coarse
        fixed = 7
        for drawn, stored in ((3 * large + small + fixed, None),
                              (4 * len(self.tris) + fixed, large + small + fixed)):
            with self.subTest(drawn=drawn, stored=stored):
                result = owner.allocate([self.large, self.small], drawn, stored, fixed, fixed)
                counts = result.counts
                self.assertGreater(result.error, 0)
                self.assertLessEqual(3 * counts["large"] + counts["small"] + fixed, drawn)
                if stored is not None:
                    self.assertLessEqual(sum(counts.values()) + fixed, stored)

    def test_tightening_never_raises_a_count(self):
        pieces = [self.large, self.small]
        tight = owner.allocate(pieces, 4 * len(self.tris), sum(self.coarse))
        loose = owner.allocate(pieces, 4 * len(self.tris), 2 * len(self.tris))
        self.assertLess(loose.error, tight.error)
        for name, count in tight.counts.items():
            self.assertLessEqual(count, loose.counts[name])

    def test_identical_pieces_keep_equal_counts_after_translation(self):
        for measured in (False, True):
            with self.subTest(measured=measured):
                pieces = [owner.Piece(name, self.positions + shift, self.tris, 1, measured)
                          for name, shift in (("local", 0), ("distant", COPY_SHIFT))]
                budget = 2 * owner.piece_count(pieces[0], self.maximum)
                result = owner.allocate(pieces, budget, None)
                self.assertEqual(result.counts["local"], result.counts["distant"])

    def test_measured_small_piece_stays_at_its_floor(self):
        result = owner.allocate([self.large, self.small], 3 * self.coarse[0] + self.coarse[1], None)
        floor = owner.FLOOR_SHARE * bounds_diagonal(self.small.positions)
        self.assertGreater(result.error, floor)
        error = owner.piece_error(self.small, result.error)
        kept = detail.level(self.small.positions, self.small.triangles, error)
        self.assertEqual(result.counts["small"], len(kept))
        self.assertLessEqual(detail.surface_error(self.small.positions, self.tris, self.small.positions, kept), floor)

    def test_rest_uses_one_floor_and_prunes_a_tiny_disconnected_part(self):
        tiny = np.array([[0., 0., 0.], [SMALL_SCALE, 0., 0.], [0., SMALL_SCALE, 0.]]) + [0, 0, 2]
        face = np.array([[0, 1, 2]])
        whole = owner.Piece("whole", np.concatenate([self.positions, tiny]),
                            np.concatenate([self.tris, face + len(self.positions)]), 1, False)
        floor = owner.piece_error(whole, np.inf)
        self.assertAlmostEqual(floor, owner.FLOOR_SHARE * bounds_diagonal(whole.positions))
        self.assertGreater(floor, owner.piece_error(owner.Piece("part", tiny, face, 1, False), np.inf))
        expected = simplify_to_error(whole.positions, whole.triangles, floor, ERROR_ABSOLUTE | PRUNE)
        self.assertTrue(np.all(expected < len(self.positions)))
        with patch.object(owner, "simplify_to_error", wraps=simplify_to_error) as simplify:
            self.assertEqual(owner.piece_count(whole, floor), len(expected))
        self.assertEqual(simplify.call_args.args[3], ERROR_ABSOLUTE | PRUNE)

    def test_counts_take_geometry_and_budgets_only(self):
        self.assertEqual(tuple(inspect.signature(owner.allocate).parameters),
                         ("pieces", "drawn", "stored", "fixed_drawn", "fixed_stored"))
        self.assertEqual(tuple(inspect.signature(owner.piece_count).parameters), ("piece", "error"))

    def test_impossible_floors_name_both_totals(self):
        with self.assertRaisesRegex(SettingsError, r"7 drawn triangles.*5 stored triangles"):
            owner.allocate([], 6, 4, fixed_drawn=7, fixed_stored=5)


    def test_actual_overrun_reallocates_within_both_limits(self):
        piece = self.large
        limit = owner.piece_count(piece, self.maximum / 2)
        growth = 2
        labels = np.zeros(len(piece.triangles), dtype=int)
        report = {}
        allocation = simplify.Allocation(
            tri_o=labels, object_names=(piece.name,), ceiling=limit,
            source=(piece.positions, piece.triangles, labels, labels), report=report,
        )

        def emit(positions, colours, faces, materials, budget, *args):
            actual = budget + growth
            return positions, colours, faces[:actual], materials[:actual]

        with patch.object(simplify, "_run_pass", side_effect=emit):
            output = simplify.simplify(piece.positions, np.zeros_like(piece.positions), piece.triangles,
                                       labels, limit, colour_deviation=1, allocation=allocation)
        self.assertEqual(report["allocation_trials"], 2)
        self.assertLessEqual(len(output[2]), limit)
        self.assertLessEqual(report["stored_total"], limit)
