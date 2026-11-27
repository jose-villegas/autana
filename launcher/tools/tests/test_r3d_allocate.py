"""Shape error allocation for prototypes and whole source objects."""

import pathlib
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

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
COPY_SHIFT = np.array([2. ** 20, -2. ** 20, 2. ** 20])
ERROR_SAMPLES = 49


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
                result = owner.allocate([self.large, self.small], drawn, stored, fixed)
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
                pieces = [owner.Piece(name, self.small.positions + shift, self.tris, 1, measured)
                          for name, shift in (("local", 0), ("distant", COPY_SHIFT))]
                budget = 2 * owner.piece_count(pieces[0], self.maximum)
                self.assertLess(budget, 2 * len(self.tris))
                result = owner.allocate(pieces, budget, None)
                self.assertEqual(result.counts["local"], result.counts["distant"])

    def test_measured_small_piece_stays_at_its_floor(self):
        small = owner.Piece("small", self.small.positions, self.tris, 1, True)
        candidates = []

        def measure(positions, triangles, error):
            kept = detail.level(positions, triangles, error)
            candidates.append((positions, kept))
            return kept

        with patch.object(owner, "level", side_effect=measure):
            result = owner.allocate([self.large, small], 3 * self.coarse[0] + self.coarse[1], None)
        floor = owner.FLOOR_SHARE * bounds_diagonal(small.positions)
        self.assertGreater(result.error, floor)
        self.assertEqual(owner.piece_error(small, result.error), floor)
        positions, kept = min(candidates, key=lambda candidate: len(candidate[1]))
        self.assertEqual(result.counts["small"], len(kept))
        self.assertLessEqual(detail.surface_error(positions, self.tris, positions, kept), floor)

    def test_the_floor_does_not_discard_a_finer_feasible_measured_level(self):
        piece = owner.Piece("prototype", self.positions, self.tris, 1, True)
        floor = owner.piece_error(piece, np.inf)
        finer_error = floor * 2 / 3
        errors = np.linspace(finer_error, floor, ERROR_SAMPLES)
        self.assertTrue(np.all(np.diff([owner.piece_count(piece, error) for error in errors]) <= 0))
        limit = owner.piece_count(piece, finer_error)
        self.assertLess(limit, len(piece.triangles))
        self.assertLessEqual(owner.piece_count(piece, floor), limit)
        self.assertLessEqual(owner.allocate([piece], limit, limit).error, finer_error)

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

    def test_impossible_floors_name_both_totals(self):
        with self.assertRaisesRegex(SettingsError, r"7 drawn triangles.*7 stored triangles"):
            owner.allocate([], 6, 4, fixed=7)


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

        def emit(positions, colours, faces, materials, budget, *args, **kwargs):
            actual = budget + growth
            return positions, colours, faces[:actual], materials[:actual]

        with patch.object(simplify, "_run_pass", side_effect=emit):
            output = simplify.simplify(piece.positions, np.zeros_like(piece.positions), piece.triangles,
                                       labels, limit, colour_deviation=1, allocation=allocation)
        self.assertEqual(report["allocation_trials"], 2)
        self.assertLessEqual(len(output[2]), limit)
        self.assertLessEqual(report["stored_total"], limit)

    def emit_scene(self, positions, faces, owners, names, limit, *, colours=None, **settings):
        report = {}
        config = simplify.Allocation(tri_o=owners, object_names=names, material_names=names,
                                     ceiling=limit, source=(positions, faces, owners, owners),
                                     report=report, **settings)
        colours = np.zeros_like(positions) if colours is None else colours
        output = simplify.simplify(positions, colours, faces, owners, limit,
                                   colour_deviation=1, allocation=config)
        return output, report

    def two_objects(self):
        small = self.small.positions + [3, 0, 0]
        p = np.concatenate([self.positions, small])
        t = np.concatenate([self.tris, self.tris + len(self.positions)])
        return p, t, np.repeat([0, 1], len(self.tris))

    def test_emitted_rest_objects_keep_their_own_caps(self):
        p, t, owners = self.two_objects()
        large = owner.Piece("large", self.positions, self.tris, 1, False)
        small = owner.Piece("small", self.small.positions, self.tris, 1, False)
        limit = sum(owner.piece_count(piece, self.maximum) for piece in (large, small)) + 16
        (q, _, faces, labels), report = self.emit_scene(p, t, owners, ("large", "small"), limit)
        self.assertGreater(report["error"], owner.piece_error(small, np.inf))
        for index, piece in enumerate((large, small)):
            moved = detail.surface_error(p, t[owners == index], q, faces[labels == index])
            self.assertLessEqual(moved, owner.piece_error(piece, report["error"]), piece.name)

    def test_retry_emits_at_the_floor_when_the_learned_excess_cannot_fit(self):
        attempts = []
        limit, floor_count, excess = 90, 85, 10

        def count(piece, error):
            return floor_count if error >= owner.piece_error(piece, np.inf) else limit

        def emit(p, c, t, m, budget, *args, **kwargs):
            attempts.append(budget)
            emitted = budget + excess if budget == limit else budget
            return p, c, t[:emitted], m[:emitted]

        owners = np.zeros(len(self.tris), dtype=int)
        with patch.object(owner, "piece_count", side_effect=count), patch.object(simplify, "_run_pass", side_effect=emit):
            (_, _, faces, _), report = self.emit_scene(self.positions, self.tris, owners, ("rest",), limit)
        self.assertEqual(attempts, [limit, floor_count])
        self.assertEqual(len(faces), floor_count)
        self.assertEqual(report["allocation_trials"], 2)

    def test_persistent_excess_runs_three_rounds_and_errors_with_both_limits(self):
        limit, actual = 90, 100
        owners = np.zeros(len(self.tris), dtype=int)
        with patch.object(simplify, "_run_pass", return_value=(
                self.positions, np.zeros_like(self.positions), self.tris[:actual], owners[:actual])) as emit:
            with self.assertRaisesRegex(SettingsError, r"100 drawn triangles \(budget 90\).*100 stored triangles \(ceiling 90\)"):
                self.emit_scene(self.positions, self.tris, owners, ("rest",), limit)
        self.assertEqual(emit.call_count, 3)

    def test_emitted_rest_prunes_a_tiny_component_and_preserves_it_when_held(self):
        near = self.positions[0] + [0, SMALL_SCALE, 0]
        tiny = near + np.array([[0, 0, 0], [SMALL_SCALE, 0, 0], [0, 0, SMALL_SCALE]])
        p = np.concatenate([self.positions, tiny])
        t = np.concatenate([self.tris, [np.arange(len(self.positions), len(p))]])
        owners = np.zeros(len(t), dtype=int)
        limit = self.coarse[0] + 12
        held = np.arange(len(t)) == len(t) - 1
        for hold in (False, True):
            with self.subTest(hold=hold):
                with patch.object(simplify, "simplify_with_update", wraps=simplify.simplify_with_update) as run:
                    (q, _, faces, _), _ = self.emit_scene(p, t, owners, ("rest",), limit, held=held & hold)
                self.assertEqual(run.call_args_list[0].args[5] & (ERROR_ABSOLUTE | PRUNE), ERROR_ABSOLUTE | PRUNE)
                self.assertEqual(bool(np.any(np.all(q[faces, 1] > 1, axis=1))), hold)

    def test_carved_overrun_lowers_the_nominal_count_before_shading(self):
        p = self.positions
        owners = np.zeros(len(self.tris), dtype=int)
        piece = owner.Piece("0", p, self.tris, 1, True)
        limit = owner.piece_count(piece, self.maximum / 2)
        growth = 2
        attempts, shapes = [], []

        def stand(positions, rgb, tris, materials, budget, *args):
            attempts.append(budget)
            actual = min(len(tris), budget + growth)
            result = SimpleNamespace(positions=positions, rgb=rgb, tris=tris[:actual],
                                     kept=np.arange(len(positions)), materials=materials[:actual], kind="test")
            return result, [], budget, 0

        def shade(p, c, t, m, **kwargs):
            return p, c, t, m

        with patch.object(simplify, "stand_in_within", side_effect=stand):
            lit = Mock(side_effect=shade)
            _, report = self.emit_scene(p, self.tris, owners, ("prototype",), limit,
                                        selection={0: {"match": {"object": "prototype"}}}, shapes=shapes, shade=lit)
        self.assertEqual(len(attempts), 2)
        self.assertLess(attempts[1], attempts[0])
        self.assertLessEqual(report["scene_total"], limit)
        self.assertLessEqual(report["stored_total"], limit)
        self.assertEqual(lit.call_count, 1)

    def test_a_carved_prototype_is_emitted_within_its_measured_floor(self):
        p, t, owners = self.two_objects()
        large = owner.Piece("large", self.positions, self.tris, 1, False)
        limit = large.floor_count + len(self.tris) + 8
        # A hard albedo boundary competes with surface error in the stand-in.
        colours = np.broadcast_to(np.where(p[:, :1] > 3, 255., 0.), p.shape).copy()
        colours[:len(self.positions)] = 0
        shapes = []
        _, report = self.emit_scene(p, t, owners, ("large", "small"), limit, colours=colours,
                                    selection={1: {"match": {"object": "small"}}}, shapes=shapes)
        (shape,) = shapes
        self.assertGreater(report["error"], self.small.floor)
        _, (placed, _, faces, _) = shape["copies"][0]
        moved = detail.surface_error(p, t[owners == 1], placed, faces)
        self.assertLessEqual(moved, owner.FLOOR_SHARE * bounds_diagonal(self.small.positions))

    def test_full_detail_keeps_the_original_counts_at_zero_error(self):
        positions = np.array([[x, y, 0.] for y in range(3) for x in range(3)])
        tris = np.array([[a, a + 1, a + 4] if side == 0 else [a, a + 4, a + 3]
                         for a in (0, 1, 3, 4) for side in (0, 1)])
        pieces = [owner.Piece(str(measured), positions, tris, 1, measured) for measured in (False, True)]
        result = owner.allocate(pieces, 2 * len(tris), 2 * len(tris))
        self.assertEqual(result.error, 0)
        self.assertEqual(list(result.counts.values()), [len(tris)] * len(pieces))
