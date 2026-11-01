"""Mesh-owned budgets, surface contacts and rigid shared simplification."""

import pathlib
import sys
import unittest
from types import SimpleNamespace
from contextlib import contextmanager
from unittest.mock import Mock, patch

import numpy as np
from scipy.spatial import ConvexHull

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.import_settings import SettingsError
from r3d import import_settings
from r3d.simplify import Allocation, simplify
from soup import edge_counts, sphere, split_sphere


@contextmanager
def watched_simplify():
    """One mock over every simplify_with_update call, the rest's passes and the carved stand-ins' alike, in order."""
    from r3d import meshopt, simplify as owner
    from r3d.objects import stand_ins
    run = Mock(wraps=meshopt.simplify_with_update)
    with patch.object(owner, 'simplify_with_update', run), patch.object(stand_ins, 'simplify_with_update', run):
        yield run

CARVED_RULE = 40  # triangles a carved mesh asks for
REST_ASK = 80  # triangles the rest asks for
ROUNDING = 3  # meshoptimizer lands near a budget, not on it


class MeshPolicyTests(unittest.TestCase):
    def test_schema(self):
        valid = {'match': {'material': 'vase*'}, 'triangles': 40, 'seam': 'lock'}
        self.assertEqual(import_settings.load_object_meshes({'meshes': [valid]}), [valid])
        for invalid in ({**valid, 'match': {'object': '*', 'material': '*'}},
                        {**valid, 'seam': 'unknown'}, {**valid, 'typo': True},
                        {**valid, 'match': {'typo': '*'}}, {**valid, 'triangles': 0}):
            with self.subTest(invalid=invalid), self.assertRaises(SettingsError):
                import_settings.load_object_meshes({'meshes': [invalid]})
        with self.assertRaisesRegex(SettingsError, r'\[\[objects.meshes\]\]'):
            import_settings.load_object_meshes({'overrides': []})
        self.assertEqual(import_settings.load_variant({'name': 'test', 'triangles': 80, 'ceiling': 120},
                                      SimpleNamespace(simplify=True), 'variant').ceiling, 120)

    def test_copy_budget_per_variant(self):
        rules = [{'match': {'material': 'vase'}, 'triangles': 40},
                 {'match': {'material': 'pot'}, 'triangles': 40, 'variants': {'lite': 7}},
                 {'match': {'material': 'leaf'}, 'triangles': 3, 'min_triangles': 2}]
        self.assertEqual(import_settings.load_object_meshes({'meshes': rules}), rules)
        steps = SimpleNamespace(simplify=True)
        full = import_settings.load_variant({'name': 'full', 'triangles': 80}, steps, 'variant')
        lite = import_settings.load_variant({'name': 'lite', 'triangles': 60, 'objects_scale': 0.25}, steps, 'variant')
        self.assertEqual([rule['triangles'] for rule in import_settings.variant_meshes(rules, full)], [40, 40, 3])
        self.assertEqual([rule['triangles'] for rule in import_settings.variant_meshes(rules, lite)], [10, 7, 2])
        self.assertEqual(rules[0]['triangles'], 40)
        for invalid in ({**rules[0], 'variants': {'lite': 0}}, {**rules[0], 'variants': 3},
                        {**rules[0], 'min_triangles': 41}):
            with self.subTest(invalid=invalid), self.assertRaises(SettingsError):
                import_settings.load_object_meshes({'meshes': [invalid]})
        for scale in (0, -1, 'half'):
            with self.subTest(scale=scale), self.assertRaises(SettingsError):
                import_settings.load_variant({'name': 'lite', 'triangles': 60, 'objects_scale': scale}, steps, 'variant')

    def test_surface_contact_without_shared_vertices(self):
        from r3d.objects.contact import contact_graph
        p = np.array([[0., 0, 0], [4, 0, 0], [0, 4, 0],
                      [1, 1, 0], [1, 1, 2], [2, 1, 2],
                      [9, 0, 0], [10, 0, 0], [9, 1, 0]])
        edges = contact_graph(p, np.arange(9).reshape(-1, 3), np.arange(3))
        self.assertEqual([(e.a, e.b) for e in edges], [(0, 1)])
        np.testing.assert_array_equal(edges[0].vertices_b, [3])
        np.testing.assert_array_equal(edges[0].vertices_a, [0, 1, 2])

    def test_arch_columns_floor_and_free_vase(self):
        from itertools import product
        from r3d.objects.contact import contact_graph
        boxes = [((-2, 2), (0, 1), (3, 4)),
                 ((-2, -1), (0, 1), (0, 3)), ((1, 2), (0, 1), (0, 3)),
                 ((-4, 4), (-2, 2), (-1, 0)), ((8, 9), (0, 1), (2, 3))]
        points, faces, owners = [], [], []
        for index, box in enumerate(boxes):
            p = np.array(list(product(*box)), dtype=float)
            t = ConvexHull(p).simplices
            points.extend(p)
            faces.extend(t + index * len(p))
            owners.extend([index] * len(t))
        args = np.asarray(points), np.asarray(faces), np.asarray(owners)
        edges = contact_graph(*args)
        self.assertEqual([(e.a, e.b) for e in edges], [(0, 1), (0, 2), (1, 3), (2, 3)])
        for a, b in zip(edges, contact_graph(*args)):
            np.testing.assert_array_equal(a.vertices_a, b.vertices_a)
            np.testing.assert_array_equal(a.vertices_b, b.vertices_b)

    def test_weld_adds_to_touched_rest_pass(self):
        report = {}
        p, t, _, allocation = split_sphere(11, 40, 'weld', report)
        with watched_simplify() as run:
            q, _, faces, _ = simplify(p, np.full_like(p, 100), t, np.zeros(len(t), dtype=int), 80,
                                       colour_deviation=1, allocation=allocation)
        self.assertEqual([call.args[3] for call in run.call_args_list], [120])
        self.assertLess(len(faces), len(t))
        self.assertEqual(report['requested_scene_total'], 120)
        self.assertEqual(report['scene_total'], len(faces))
        counts = edge_counts(faces)
        np.testing.assert_array_equal(counts, np.full(len(counts), 2))

    def test_no_rules_parity_with_material_shares(self):
        p, t = sphere(7)
        labels = np.arange(len(t)) % 2
        for sealed in (False, True):
            args = dict(colour_deviation=1, reserved=[({0}, 0.3)], seal_seams=sealed)
            plain = simplify(p, np.full_like(p, 100), t, labels, 80, **args)
            allocated = simplify(p, np.full_like(p, 100), t, labels, 80,
                                 allocation=Allocation(tri_o=np.zeros(len(t), dtype=int)), **args)
            for a, b in zip(plain, allocated):
                self.assertEqual(a.tobytes(), b.tobytes())

    def test_mesh_budget_does_not_subtract_from_rest(self):
        points, faces, owners, allocation = self._three_spheres()
        report = allocation.report = {}
        *_, labels = simplify(points, np.full_like(points, 100), faces, owners, 80,
                              colour_deviation=1, allocation=allocation)
        self.assertEqual([np.count_nonzero(labels == i) for i in range(3)], [40, 40, 80])
        self.assertEqual(report['scene_total'], 160)
        self.assertTrue(all(row['policy'] == 'free' for row in report['meshes']))
        # Carved meshes are paid first: under a ceiling the rest shrinks to what is left of it.
        allocation.ceiling = 159
        *_, labels = simplify(points, np.full_like(points, 100), faces, owners, 80, colour_deviation=1, allocation=allocation)
        self.assertEqual([np.count_nonzero(labels == i) for i in range(2)], [40, 40])
        self.assertLessEqual(len(labels), 159)
        allocation.ceiling = 79
        with self.assertRaisesRegex(SettingsError, 'ceiling'):
            simplify(points, np.full_like(points, 100), faces, owners, 80, colour_deviation=1, allocation=allocation)
        allocation.ceiling = None
        allocation.meshes = ({'match': {'object': 'missing'}, 'triangles': 40},)
        with self.assertRaisesRegex(SettingsError, 'matches no'):
            simplify(points, np.full_like(points, 100), faces, owners, 80, colour_deviation=1, allocation=allocation)
        allocation.meshes = ({'match': {'material': 'prop*'}, 'triangles': 40},)
        allocation.material_names = ('prop_a', 'prop_b', 'rest')
        *_, labels = simplify(points, np.full_like(points, 100), faces, owners, 80, colour_deviation=1, allocation=allocation)
        self.assertEqual([np.count_nonzero(labels == i) for i in range(3)], [40, 40, 80])

    def test_lock_keeps_both_sides_and_free_keeps_the_rests(self):
        for seed in range(3):
            p, t = sphere(seed)
            owners = (p[t].mean(axis=1)[:, 0] > 0).astype(int)
            border = np.intersect1d(np.unique(t[owners == 0]), np.unique(t[owners == 1]))
            results = {}
            for seam in ('free', 'lock'):
                allocation = Allocation(tri_o=owners, object_names=('left', 'right'), material_names=('plain',),
                                        meshes=({'match': {'object': 'left'}, 'triangles': 60, 'seam': seam},))
                q, _, out, labels = simplify(p, np.full_like(p, 100), t, owners, 60,
                                             colour_deviation=1, allocation=allocation)
                sides = [q[np.unique(out[labels == i])] for i in range(2)]
                kept = [all(np.any(np.all(side == v.astype(np.float32), axis=1)) for v in p[border]) for side in sides]
                results[seam] = kept
                if seam == 'lock':
                    _, inverse = np.unique(q, axis=0, return_inverse=True)
                    welded = inverse[out]
                    counts = edge_counts(welded)
                    np.testing.assert_array_equal(counts, np.full(len(counts), 2))
            self.assertEqual(results['lock'], [True, True])
            self.assertTrue(results['free'][1])  # the rest keeps its border; a welded vertex is the mesh's too

    def test_ceiling_counts_lock_excess(self):
        report = {}
        p, t, _, allocation = split_sphere(12, 10, 'lock', report)
        simplify(p, np.full_like(p, 100), t, np.zeros(len(t), dtype=int), 10, colour_deviation=1, allocation=allocation)
        self.assertGreater(report['scene_total'], report['requested_scene_total'])
        self.assertGreater(sum(row['locked_border_excess'] for row in report['passes']), 0)
        allocation.ceiling = report['scene_total'] - 1
        with self.assertRaisesRegex(SettingsError, 'emitted triangles'):
            simplify(p, np.full_like(p, 100), t, np.zeros(len(t), dtype=int), 10, colour_deviation=1, allocation=allocation)

    def test_shared_lock_unions_contacts_from_all_copies(self):
        from scipy.spatial import cKDTree
        from r3d.objects.contact import contact_graph
        p, t = sphere(13)
        reflected = p * [-1, 1, 1] + [6, 0, 0]
        anchors = np.array([p[0], reflected[1]])
        support = np.concatenate([np.array([v, v * [1, 1, 1] + [0, 0, 2], v + [0, 2, 0]]) for v in anchors])
        points = np.concatenate([p, reflected, support])
        faces = np.concatenate([t, t[:, ::-1] + len(p), np.arange(2 * len(p), 2 * len(p) + 6).reshape(-1, 3)])
        owners = np.concatenate([np.repeat([0, 1], len(t)), [2, 2]])
        locks = np.zeros(len(points), dtype=bool)
        for edge in contact_graph(points, faces, owners):
            locks[edge.vertices_a] = True
            locks[edge.vertices_b] = True
        expected = locks[:len(p)] | locks[len(p):2 * len(p)]
        self.assertGreater(np.count_nonzero(expected), np.count_nonzero(locks[:len(p)]))
        allocation = Allocation(tri_o=owners, object_names=('vase_a', 'vase_b', 'rest'), material_names=('plain',),
                                meshes=({'match': {'object': 'vase*'}, 'triangles': 40},))
        with watched_simplify() as run:
            simplify(points, np.full_like(points, 100), faces, np.zeros(len(faces), dtype=int), 20,
                     colour_deviation=1, allocation=allocation)
        shared = next(call for call in run.call_args_list if len(call.args[0]) == len(p))
        centre = (p.min(axis=0) + p.max(axis=0)) / 2
        _, mapping = cKDTree(p).query(shared.args[0] + centre)
        np.testing.assert_array_equal(shared.kwargs['vertex_lock'], expected[mapping])
        # Only the first placement touches; the isolated copy shares its local locks.
        with watched_simplify() as run:
            simplify(points, np.full_like(points, 100), faces[:-1], np.zeros(len(faces) - 1, dtype=int), 20,
                     colour_deviation=1, allocation=Allocation(tri_o=owners[:-1], object_names=allocation.object_names,
                                                              material_names=allocation.material_names, meshes=allocation.meshes))
        shared = [call for call in run.call_args_list if len(call.args[0]) == len(p)]
        self.assertEqual(len(shared), 1)
        self.assertTrue(np.any(shared[0].kwargs['vertex_lock']))

    def test_weld_pins_vertex_to_face_contact(self):
        p = np.array([[0., 0, 0], [4, 0, 0], [0, 4, 0], [1, 1, 0], [1, 1, 2], [2, 1, 2]])
        t = np.arange(6).reshape(-1, 3)
        allocation = Allocation(tri_o=np.arange(2), object_names=('floor', 'column'), material_names=('plain',),
                                meshes=({'match': {'object': 'column'}, 'triangles': 1, 'seam': 'weld'},))
        with watched_simplify() as run:
            simplify(p, np.full_like(p, 100), t, np.zeros(len(t), dtype=int), 1, colour_deviation=1, allocation=allocation)
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[3], 2)
        np.testing.assert_array_equal(run.call_args.kwargs['vertex_lock'], [True, True, True, True, False, False])

    def test_shared_bake_uses_albedo_then_placed_light(self):
        from r3d import mesh_import
        p, t = sphere(9)
        points = np.concatenate([p, p * [-1, 1, 1] + [6, 0, 0]])
        faces = np.concatenate([t, t[:, ::-1] + len(p)])
        src = SimpleNamespace(p=points, tri_v=faces, tri_t=None, uv=None, tri_m=np.zeros(len(faces), dtype=int),
                              tri_o=np.repeat([0, 1], len(t)), names=['plain'], object_names=['vase_a', 'vase_b'],
                              textures=[None], materials={}, colors=None)
        settings = SimpleNamespace(seed=0, position_scale=None, alpha_keep=None, thin=None, double_sided=set(),
                     meshes=[{'match': {'object': 'vase*'}, 'triangles': 40}],
                     simplify=SimpleNamespace(dense_edge=10, props=set(), props_share=0, seal_seams=False, colour_deviation=1))
        job = SimpleNamespace(settings=settings, renderer=SimpleNamespace(visibility=None, variant=SimpleNamespace(name="mesh", triangles=80, objects_scale=1.0)),
                              bake=SimpleNamespace(indirect=None, ray_offset=0.01, ao=None, colour_merge_step=6))
        scene = SimpleNamespace(lights=[{'type': 'directional', 'direction': [1, 0, 0], 'color': [1, 1, 1], 'intensity': 1}],
                                tonemap_white=1, indirect=SimpleNamespace(intensity=1))
        query = SimpleNamespace(blocked=lambda origin, direction: np.zeros(len(origin), dtype=bool))
        with patch.object(mesh_import, 'load_source', return_value=src), \
             patch.object(mesh_import, 'RayQuery', return_value=query) as rays, \
             watched_simplify() as run, \
             patch.object(mesh_import, 'shade_placed', wraps=mesh_import.shade_placed) as shade:
            result = mesh_import.bake_geometry(job, scene)
        self.assertEqual(run.call_count, 1)
        np.testing.assert_array_equal(run.call_args.args[1], np.full_like(run.call_args.args[0], 255))
        self.assertEqual(shade.call_count, 2)
        self.assertIs(shade.call_args_list[0].args[-2], query)
        np.testing.assert_array_equal(rays.call_args.args[1], faces)
        left = result.positions[:, 0] < 3
        self.assertFalse(np.array_equal(np.sort(result.rgb[left], axis=0), np.sort(result.rgb[~left], axis=0)))
        self.assertEqual(len(result.tris), 80)

        settings.thin = SimpleNamespace(material='plain', keep=0.7)
        job.renderer.visibility = SimpleNamespace(source='camera_region')
        src.tri_v = np.concatenate([t, t[::-1, ::-1] + len(p)])
        visible = np.repeat([True, False], len(t))
        with patch.object(mesh_import, 'load_source', return_value=src), \
             patch.object(mesh_import, 'RayQuery', return_value=query), \
             patch.object(mesh_import, 'visible_triangles', return_value=visible), \
             watched_simplify() as run:
            result = mesh_import.bake_geometry(job, scene)
        self.assertEqual(run.call_count, 1)
        self.assertLess(len(run.call_args.args[2]), len(t))
        self.assertEqual(result.mesh_report['meshes'][0]['copies'], 2)
        self.assertTrue(np.any(result.positions[:, 0] > 3))

    def test_shared_mirror_decimates_once_and_places_before_shading(self):
        p, t = sphere(8)
        reflected = p * [-1, 1, 1] + [6, 0, 0]
        points = np.concatenate([p, reflected])
        faces = np.concatenate([t, t[:, ::-1] + len(p)])
        owners = np.repeat([0, 1], len(t))
        shaded = []
        def shade(q, c, out, labels):
            shaded.append(q.copy())
            return q, np.full_like(q, q[:, 0].mean()), out, labels
        allocation = Allocation(tri_o=owners, object_names=('vase_a', 'vase_b'), material_names=('plain',),
                                meshes=({'match': {'object': 'vase*'}, 'triangles': 40},), shade=shade)
        with watched_simplify() as run:
            q, c, out, _ = simplify(points, np.full_like(points, 100), faces, np.zeros(len(faces), dtype=int), 80,
                                     colour_deviation=1, allocation=allocation)
        self.assertEqual(run.call_count, 1)
        self.assertEqual(len(shaded), 2)
        n = len(q) // 2
        np.testing.assert_allclose(q[:n] * [-1, 1, 1] + [6, 0, 0], q[n:], atol=1e-6)
        np.testing.assert_array_equal(out[:40][:, ::-1] + n, out[40:])
        self.assertFalse(np.array_equal(c[:n], c[n:]))

    # Carved meshes are charged at what they emit; the rest scales into what is left of the ceiling.

    @staticmethod
    def _three_spheres():
        """Two mirrored carved copies (one shared pass) and a rest sphere."""
        p, t = sphere(4)
        rest_p, rest_t = sphere(6)
        points = np.concatenate([p, p * [-1, 1, 1] + [6, 0, 0], rest_p + [12, 0, 0]])
        faces = np.concatenate([t, t[:, ::-1] + len(p), rest_t + 2 * len(p)])
        owners = np.concatenate([np.repeat([0, 1], len(t)), np.full(len(rest_t), 2)])
        allocation = Allocation(tri_o=owners, object_names=('vase_a', 'vase_b', 'rest'), material_names=('plain',),
                                meshes=({'match': {'object': 'vase*'}, 'triangles': CARVED_RULE},))
        return points, faces, owners, allocation

    @staticmethod
    def _lone_triangle_rest():
        """A carved sphere and a one-triangle rest that touches nothing."""
        p, t = sphere(5)
        points = np.concatenate([p, [[20., 0, 0], [21, 0, 0], [20, 1, 0]]])
        faces = np.concatenate([t, [[len(p), len(p) + 1, len(p) + 2]]])
        owners = np.concatenate([np.zeros(len(t), dtype=int), [1]])
        allocation = Allocation(tri_o=owners, object_names=('vase', 'rest'), material_names=('plain',),
                                meshes=({'match': {'object': 'vase'}, 'triangles': CARVED_RULE},))
        return points, faces, owners, allocation

    def _run(self, points, faces, labels, rest, allocation, **args):
        """Simplify; return the output labels and the budget of every simplify_with_update call, in order."""
        with watched_simplify() as run:
            try:
                *_, out = simplify(points, np.full_like(points, 100), faces, labels, rest, colour_deviation=1,
                                   allocation=allocation, **args)
            finally:
                self.budgets = [call.args[3] for call in run.call_args_list]
        return out, self.budgets

    def test_ceiling_charges_locked_seam_overshoot_not_the_rule_budget(self):
        report = {}
        rest_ask = REST_ASK + CARVED_RULE  # above the floor the locked border leaves the rest
        p, t, _, allocation = split_sphere(12, CARVED_RULE, 'lock', report)
        labels = np.zeros(len(t), dtype=int)
        simplify(p, np.full_like(p, 100), t, labels, rest_ask, colour_deviation=1, allocation=allocation)
        carved = report['passes'][0]['output_triangles']
        self.assertGreater(carved, CARVED_RULE)
        allocation.ceiling = carved + rest_ask - 2
        out, budgets = self._run(p, t, labels, rest_ask, allocation)
        self.assertLessEqual(len(out), allocation.ceiling)
        self.assertEqual(budgets[-1], allocation.ceiling - carved)
        self.assertLess(budgets[-1], allocation.ceiling - CARVED_RULE)

    def test_ceiling_charges_every_shared_copy(self):
        points, faces, owners, allocation = self._three_spheres()
        copies = 2
        allocation.ceiling = copies * CARVED_RULE + REST_ASK // 2
        out, budgets = self._run(points, faces, np.zeros(len(faces), dtype=int), REST_ASK, allocation)
        self.assertEqual(len(budgets), 2)  # one shared pass for both copies, then the rest
        self.assertEqual(budgets[-1], REST_ASK // 2)
        self.assertLessEqual(len(out), allocation.ceiling)

    def test_rest_gets_the_leftover_of_the_ceiling(self):
        points, faces, owners, allocation = self._three_spheres()
        allocation.ceiling = 2 * CARVED_RULE + REST_ASK - 1
        out, budgets = self._run(points, faces, np.zeros(len(faces), dtype=int), REST_ASK, allocation)
        leftover = allocation.ceiling - 2 * CARVED_RULE
        self.assertEqual(budgets[-1], leftover)
        self.assertLessEqual(len(out), allocation.ceiling)
        self.assertAlmostEqual(len(out) - 2 * CARVED_RULE, leftover, delta=ROUNDING)

    def test_rest_parts_scale_proportionally_and_none_drops_to_zero(self):
        p, t = sphere(7)
        points = np.concatenate([p, p + [6, 0, 0]])
        faces = np.concatenate([t, t + len(p)])
        owners = np.repeat([0, 1], len(t))
        labels = np.concatenate([np.zeros(len(t), dtype=int), np.arange(len(t)) % 2])
        share = 0.3
        asks = [round(REST_ASK * share), REST_ASK - round(REST_ASK * share)]

        def run(ceiling):
            allocation = Allocation(tri_o=owners, object_names=('vase', 'rest'), material_names=('a', 'b'),
                                    meshes=({'match': {'object': 'vase'}, 'triangles': CARVED_RULE},), ceiling=ceiling)
            return self._run(points, faces, labels, REST_ASK, allocation, reserved=[({0}, share)])

        _, budgets = run(None)
        self.assertEqual(sorted(budgets[1:]), sorted(asks))
        room = REST_ASK // 2
        out, budgets = run(CARVED_RULE + room)
        self.assertEqual(sorted(budgets[1:]), sorted(ask * room // REST_ASK for ask in asks))
        self.assertLessEqual(len(out), CARVED_RULE + room)
        tight = len(asks)  # one triangle per rest part is all the ceiling leaves
        _, budgets = run(CARVED_RULE + tight)
        self.assertEqual(budgets[1:], [1] * tight)

    def test_ceiling_boundaries_leave_the_rest_within_its_ask(self):
        total_ask = 2 * CARVED_RULE + REST_ASK
        for ceiling in (total_ask, total_ask + REST_ASK):
            points, faces, owners, allocation = self._three_spheres()
            allocation.ceiling = ceiling
            out, budgets = self._run(points, faces, np.zeros(len(faces), dtype=int), REST_ASK, allocation)
            self.assertEqual(budgets[-1], REST_ASK, ceiling)
            self.assertEqual(len(out) - 2 * CARVED_RULE, REST_ASK, ceiling)

    def test_ceiling_of_carved_plus_one_triangle_per_rest_part_passes(self):
        points, faces, owners, allocation = self._lone_triangle_rest()
        allocation.ceiling = CARVED_RULE + 1
        out, _ = self._run(points, faces, owners, 1, allocation)
        self.assertEqual(len(out), allocation.ceiling)

    def test_carved_alone_over_ceiling_raises_before_the_rest_is_simplified(self):
        points, faces, owners, allocation = self._lone_triangle_rest()
        allocation.ceiling = CARVED_RULE
        with self.assertRaisesRegex(SettingsError, 'ceiling'):
            self._run(points, faces, owners, 1, allocation)
        self.assertEqual(len(self.budgets), 1)  # the carved pass only

    def test_variant_triangles_may_not_exceed_its_ceiling(self):
        steps = SimpleNamespace(simplify=True)
        ceiling = 120
        for triangles, accepted in ((ceiling - 1, True), (ceiling, True), (ceiling + 1, False)):
            value = {'name': 'v', 'triangles': triangles, 'ceiling': ceiling}
            with self.subTest(triangles=triangles):
                if accepted:
                    self.assertEqual(import_settings.load_variant(value, steps, 'variant').triangles, triangles)
                else:
                    with self.assertRaisesRegex(SettingsError, 'ceiling'):
                        import_settings.load_variant(value, steps, 'variant')


if __name__ == '__main__':
    unittest.main()
