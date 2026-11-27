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
from r3d.objects import allocate as owner, contact, detail
from test_r3d_detail import sphere
from test_r3d_objects import POINTS, TRIANGLES

SPHERE_RINGS = 8
SMALL_SCALE = 0.01
COPY_SHIFT = np.array([2. ** 20, -2. ** 20, 2. ** 20])
ERROR_SAMPLES = 49
POOLED_ERROR = 0.12
FLOORED_RADIUS = 0.1
TOUCHING_RADIUS = 0.2
SPLIT_POLE_GAP = 0.00035
FRACTIONAL_PLACEMENT = 128.00025


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

    def emit_scene(self, positions, faces, owners, names, limit, *, colours=None, materials=None, **settings):
        report = {}
        ceiling = settings.pop("ceiling", limit)
        materials = owners if materials is None else materials
        config = simplify.Allocation(tri_o=owners, object_names=names, material_names=names,
                                     ceiling=ceiling, source=(positions, faces, materials, owners),
                                     report=report, **settings)
        colours = np.zeros_like(positions) if colours is None else colours
        output = simplify.simplify(positions, colours, faces, materials, limit,
                                   colour_deviation=1, allocation=config)
        return output, report

    def two_objects(self, radius=SMALL_SCALE, offset=(3, 0, 0)):
        small = self.positions * radius + offset
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

    def test_pooled_rest_error_limits_each_object_before_its_count_target(self):
        p, t, owners = self.two_objects(radius=1)
        target = len(self.tris) // 4
        nominal = owner.SharedError(POOLED_ERROR, {"0": target, "1": target})
        with patch.object(owner, "allocate", return_value=nominal):
            (q, _, faces, labels), report = self.emit_scene(p, t, owners, ("left", "right"), len(t))
        (pooled,) = report["passes"]
        self.assertEqual(pooled["objects"], ["left", "right"])
        self.assertLess(len(faces), len(t))
        for index, name in enumerate(pooled["objects"]):
            moved = detail.surface_error(p, t[owners == index], q, faces[labels == index])
            self.assertLessEqual(moved, POOLED_ERROR, name)
        self.assertGreater(len(faces), pooled["budget"])

    def test_capped_rest_error_stops_the_pass_before_its_count_target(self):
        p, t, owners = self.two_objects(radius=FLOORED_RADIUS)
        target = len(self.tris) // 8
        nominal = owner.SharedError(POOLED_ERROR, {"0": 2 * target, "1": target})
        with patch.object(owner, "allocate", return_value=nominal):
            (q, _, faces, labels), report = self.emit_scene(p, t, owners, ("large", "small"), len(t))
        small = faces[labels == 1]
        self.assert_capped_rest_within_floor(p, t[owners == 1], q, small, report["error"])
        self.assertGreater(len(small), target)
        self.assertLess(len(small), len(self.tris))
        self.assertEqual(report["passes"][1]["objects"], ["small"])

    def test_a_capped_rest_object_and_its_neighbour_keep_all_contact_vertices(self):
        p, t, owners = self.two_objects(radius=TOUCHING_RADIUS, offset=(0, 1 + TOUCHING_RADIUS, 0))
        (edge,) = contact.contact_graph(p, t, owners)
        target = len(self.tris) // 4
        nominal = owner.SharedError(self.maximum, {"0": 2 * target, "1": target})
        held = np.arange(len(t)) == len(self.tris)
        with patch.object(owner, "allocate", return_value=nominal):
            (q, _, faces, labels), report = self.emit_scene(p, t, owners, ("large", "small"), len(t), held=held)
        for index, vertices in ((edge.a, edge.vertices_a), (edge.b, edge.vertices_b)):
            self.assertGreater(len(vertices), 0)
            emitted = {tuple(point) for point in q[np.unique(faces[labels == index])]}
            source = {tuple(point) for point in p[vertices].astype(np.float32)}
            self.assertTrue(source <= emitted, f"{report['passes'][index]['objects']}: {len(source - emitted)} contact vertices lost")
        self.assert_capped_rest_within_floor(p, t[owners == 1], q, faces[labels == 1], report["error"])
        self.assertEqual(report["passes"][1]["budget"], target + 1)

    def assert_capped_rest_within_floor(self, p, source, q, emitted, shared_error):
        floor = owner.FLOOR_SHARE * bounds_diagonal(p[np.unique(source)])
        self.assertLess(floor, shared_error)
        self.assertLessEqual(detail.surface_error(p, source, q, emitted), floor)

    def test_rest_reuses_contact_discovery_and_excludes_carved_edges(self):
        p, t, owners = self.two_objects(radius=TOUCHING_RADIUS, offset=(0, 1 + TOUCHING_RADIUS, 0))
        # The prototype touches the opposite pole of the large sphere.
        points = np.concatenate([p, POINTS[:, [0, 2, 1]] * [1, -1, 1] + [0, -1, 0]])
        faces = np.concatenate([t, TRIANGLES + len(p)])
        labels = np.concatenate([owners, np.full(len(TRIANGLES), 2)])
        target = len(self.tris) // 2
        nominal = owner.SharedError(self.maximum, {"0": target, "1": target, "2": len(TRIANGLES)})
        selection = {2: {"match": {"object": "prototype"}}}
        with patch.object(owner, "allocate", return_value=nominal), \
                patch.object(contact, "contact_graph", wraps=contact.contact_graph) as graph, \
                patch.object(simplify, "_rest_pass", wraps=simplify._rest_pass) as rest:
            _, report = self.emit_scene(points, faces, labels, ("large", "small", "prototype"), len(faces),
                                        selection=selection, shapes=[])
        self.assertEqual({(row["a"], row["b"]) for row in report["contacts"]},
                         {("large", "small"), ("large", "prototype")})
        graph.assert_called_once()
        self.assertEqual([(edge.a, edge.b) for edge in rest.call_args.args[6]], [(0, 1)])

    def test_placement_preserves_the_rest_counts_through_simplify(self):
        p = np.vstack([self.positions, self.positions[0] + [SPLIT_POLE_GAP, 0, 0]])
        t = self.tris.copy()
        # A split pole fits one local weld cell and straddles a world-space cell boundary.
        split = np.flatnonzero(np.any(t == 0, axis=1))[:SPHERE_RINGS // 2]
        t[split] = np.where(t[split] == 0, len(self.positions), t[split])
        owners = np.zeros(len(t), dtype=int)
        limit = 2 * len(t) // 3
        counts = []
        for offset in (0, FRACTIONAL_PLACEMENT):
            (_, _, faces, _), report = self.emit_scene(p + offset, t, owners, ("rest",), limit)
            counts.append((report["rest_budget"], len(faces)))
        local, distant = counts
        self.assertLess(local[1], len(t))
        self.assertEqual(distant, local)

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

    def test_cards_count_against_drawn_on_every_trial(self):
        self.assert_cards_overrun(drawn=True)

    def test_cards_count_against_stored_on_every_trial(self):
        self.assert_cards_overrun(drawn=False)

    def assert_cards_overrun(self, *, drawn):
        cards = 4
        emitted = len(self.tris)
        limit = emitted + cards - 1
        room = emitted + cards
        budget, ceiling = (limit, room) if drawn else (room, limit)
        owners = np.zeros(emitted, dtype=int)
        with patch.object(simplify, "_run_pass", return_value=(
                self.positions, np.zeros_like(self.positions), self.tris, owners)) as emit:
            (_, _, faces, _), report = self.emit_scene(
                self.positions, self.tris, owners, ("rest",), budget, ceiling=ceiling)
            self.assertEqual((len(faces), report["allocation_trials"]), (emitted, 1))
            emit.reset_mock()
            with self.assertRaisesRegex(SettingsError, rf"{room} drawn triangles \(budget {budget}\).*"
                                                       rf"{room} stored triangles \(ceiling {ceiling}\)"):
                self.emit_scene(self.positions, self.tris, owners, ("rest",), budget, ceiling=ceiling, fixed=cards)
        self.assertEqual(emit.call_count, simplify.ALLOCATION_TRIALS)

    def test_a_stored_only_overrun_retries_at_the_finest_feasible_error(self):
        piece = owner.Piece("0", self.positions, self.tris, 1, False)
        ceiling = owner.piece_count(piece, piece.floor / 2)
        excess = 2
        expected = owner.allocate([piece], len(self.tris), ceiling, fixed=excess)
        owners = np.zeros(len(self.tris), dtype=int)

        def emit(p, c, t, m, budget, *args, **kwargs):
            return p, c, t[:budget + excess], m[:budget + excess]

        with patch.object(simplify, "_run_pass", side_effect=emit) as run:
            (_, _, faces, _), report = self.emit_scene(
                self.positions, self.tris, owners, ("rest",), len(self.tris), ceiling=ceiling)
        self.assertEqual(run.call_count, 2)
        first, retry = [call.args[4] for call in run.call_args_list]
        self.assertEqual(first, ceiling)
        self.assertEqual(retry, expected.counts["0"])
        self.assertEqual(report["error"], expected.error)
        self.assertEqual(len(faces), retry + excess)
        self.assertLessEqual(report["stored_total"], ceiling)

    def test_infeasible_floors_fail_before_emitting(self):
        piece = owner.Piece("0", self.positions, self.tris, 1, False)
        owners = np.zeros(len(self.tris), dtype=int)
        limit = piece.floor_count - 1
        with patch.object(simplify, "_run_pass", wraps=simplify._run_pass) as emit:
            with self.assertRaisesRegex(SettingsError, rf"drawn triangles \(budget {limit}\)"):
                self.emit_scene(self.positions, self.tris, owners, ("rest",), limit)
        self.assertEqual(emit.call_count, 0)

    def test_bisection_reaches_the_finest_feasible_error_to_tolerance(self):
        piece = owner.Piece("rest", self.positions, self.tris, 1, False)
        threshold = 0.51 * piece.floor
        limit = len(self.tris) - 2

        def count(piece, error):
            return limit if error >= threshold else len(piece.triangles)

        with patch.object(owner, "piece_count", side_effect=count):
            result = owner.allocate([piece], limit, None)
        self.assertGreaterEqual(result.error, threshold)
        self.assertLessEqual(result.error - threshold, threshold * owner.BISECTION_TOLERANCE)

    def test_emitted_rest_prunes_a_tiny_component_and_preserves_it_when_held(self):
        near = self.positions[0] + [0, SMALL_SCALE, 0]
        tiny = near + np.array([[0, 0, 0], [SMALL_SCALE, 0, 0], [0, 0, SMALL_SCALE]])
        p = np.concatenate([self.positions, tiny])
        t = np.concatenate([self.tris, [np.arange(len(self.positions), len(p))]])
        owners = np.zeros(len(t), dtype=int)
        limit = self.coarse[0] + 12
        for face in (0, len(t) - 1):
            held = np.arange(len(t)) == face
            hold_tiny = face == len(t) - 1
            with self.subTest(hold_tiny=hold_tiny):
                with patch.object(simplify, "simplify_with_update", wraps=simplify.simplify_with_update) as run:
                    (q, _, faces, _), report = self.emit_scene(p, t, owners, ("rest",), limit, held=held)
                options = [call.args[5] & (ERROR_ABSOLUTE | PRUNE) for call in run.call_args_list]
                per_trial = [ERROR_ABSOLUTE | PRUNE, ERROR_ABSOLUTE] if hold_tiny else [ERROR_ABSOLUTE | PRUNE]
                self.assertEqual(bool(np.any(np.all(q[faces, 1] > 1, axis=1))), hold_tiny)
                self.assertEqual(options, per_trial * report["allocation_trials"])

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

    def test_mirrored_copies_bind_the_stored_limit_per_variant(self):
        original = self.positions * [1, 2, 3]
        original[0] += [0.12, 0.23, 0.34]
        p = np.concatenate([original, original * [-1, 1, 1] + [8, 0, 0]])
        t = np.concatenate([self.tris, self.tris[:, ::-1] + len(original)])
        owners = np.repeat([0, 1], len(self.tris))
        names = ("original", "mirrored")
        selection = dict.fromkeys(range(len(names)), {"match": {"material": "shared"}})
        shapes = []
        ceiling = len(self.tris)
        _, report = self.emit_scene(p, t, owners, names, len(t), ceiling=ceiling, shapes=shapes,
                                    selection=selection, materials=np.zeros(len(t), dtype=int))
        (shape,) = shapes
        (carved,) = report["meshes"]
        self.assertEqual({instance.mirrored for instance, _ in shape["copies"]}, {False, True})
        self.assertEqual(carved["stored_meshes"], 2)
        self.assertEqual(report["allocation_trials"], 2)
        self.assertLess(report["scene_total"], len(t))
        self.assertEqual(report["stored_total"], 2 * carved["output_triangles"])
        self.assertLessEqual(report["stored_total"], ceiling)

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
