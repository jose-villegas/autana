"""Surface contacts and rigid shared simplification."""

import pathlib
import sys
import unittest
from types import SimpleNamespace
from contextlib import contextmanager
from unittest.mock import Mock, patch

import numpy as np
from scipy.spatial import ConvexHull

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.import_settings import SettingsError, LIGHT_MODE_BAKED
from r3d import import_settings
from r3d.simplify import Allocation, simplify
from r3d.objects.object_error import nearest_source
from soup import Open, edge_counts, sphere, split_sphere, subdivided
from tests.r3d_env import needs_llvm  # noqa: E402


@contextmanager
def watched_simplify():
    """One mock over every simplify_with_update call, the rest's passes and the carved stand-ins' alike, in order."""
    from r3d import meshopt, simplify as owner
    from r3d.objects import stand_ins
    run = Mock(wraps=meshopt.simplify_with_update)
    with patch.object(owner, 'simplify_with_update', run), patch.object(stand_ins, 'simplify_with_update', run):
        yield run

STRETCH = np.array([2.0, 1.0, 0.5])  # a test shape's axes, far enough apart to be told apart
BEND = 0.6  # x shifts by BEND * y^2, so no mirror or half turn maps the shape onto itself


def bent(points):
    """Unit-sphere points stretched along the axes and bent."""
    points = points * STRETCH
    return points + np.outer(BEND * points[:, 1] ** 2, [1, 0, 0])


class MeshPolicyTests(unittest.TestCase):
    def test_schema(self):
        valid = {'match': {'material': 'vase*'}, 'seam': 'lock'}
        self.assertEqual(import_settings.load_object_meshes({'meshes': [valid]}), [valid])
        for shape in ('exact', 'surface'):
            self.assertEqual(import_settings.load_object_meshes({'meshes': [{**valid, 'shape': shape}]})[0]['shape'],
                             shape)
        for invalid in ({**valid, 'match': {'object': '*', 'material': '*'}},
                        {**valid, 'seam': 'unknown'}, {**valid, 'typo': True}, {**valid, 'shape': 'loose'}, {**valid, 'shape': True},
                        {**valid, 'match': {'typo': '*'}}, {}):
            with self.subTest(invalid=invalid), self.assertRaises(SettingsError):
                import_settings.load_object_meshes({'meshes': [invalid]})
        with self.assertRaisesRegex(SettingsError, r'\[\[objects.meshes\]\]'):
            import_settings.load_object_meshes({'overrides': []})
        self.assertEqual(import_settings.load_variant({'name': 'test', 'triangles': 121, 'ceiling': 120},
                                      SimpleNamespace(simplify=True), 'variant').ceiling, 120)


    def test_surface_contact_without_shared_vertices(self):
        from r3d.objects.contact import contact_graph
        p = np.array([[0., 0, 0], [4, 0, 0], [0, 4, 0],
                      [1, 1, 0], [1, 1, 2], [2, 1, 2],
                      [9, 0, 0], [10, 0, 0], [9, 1, 0]])
        edges = contact_graph(p, np.arange(9).reshape(-1, 3), np.arange(3))
        self.assertEqual([(e.a, e.b) for e in edges], [(0, 1)])
        np.testing.assert_array_equal(edges[0].vertices_b, [3])
        np.testing.assert_array_equal(edges[0].vertices_a, [0, 1, 2])

    def test_a_vertex_on_one_face_corner_is_shared_whatever_else_it_touches(self):
        from r3d.objects.contact import contact_graph
        # Object 0's corner at the origin is a corner of object 1's first face and lies inside its upright second face.
        p = np.array([[0., 0, 0], [-3, 0, 0], [0, -3, 0],
                      [0., 0, 0], [2, 0, 1], [0, 2, 1],
                      [-1., 0, -1], [1, 0, -1], [0, 0, 1]])
        tris, owners = np.array([[0, 1, 2], [3, 4, 5], [6, 7, 8]]), np.array([0, 1, 1])
        for batch in (1, 1 << 20):
            with self.subTest(batch=batch), patch('r3d.objects.contact.PAIR_BATCH', batch):
                edges = contact_graph(p, tris, owners)
                self.assertEqual([(e.a, e.b, e.unshared) for e in edges], [(0, 1, False)])

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
        # One candidate pair per batch finds the same contacts as one batch for all.
        with patch('r3d.objects.contact.PAIR_BATCH', 1):
            for a, b in zip(edges, contact_graph(*args), strict=True):
                self.assertEqual((a.a, a.b, a.unshared), (b.a, b.b, b.unshared))
                np.testing.assert_array_equal(a.vertices_a, b.vertices_a)
                np.testing.assert_array_equal(a.vertices_b, b.vertices_b)

    def test_weld_adds_to_touched_rest_pass(self):
        report = {}
        p, t, _, allocation = split_sphere(11, 'weld', report)
        with watched_simplify() as run:
            q, _, faces, _ = simplify(p, np.full_like(p, 100), t, np.zeros(len(t), dtype=int), len(t),
                                       colour_deviation=1, allocation=allocation)
        self.assertEqual([call.args[3] for call in run.call_args_list], [report['rest_budget']])
        self.assertEqual(report['scene_total'], len(faces))
        counts = edge_counts(faces)
        np.testing.assert_array_equal(counts, np.full(len(counts), 2))



    def test_lock_keeps_both_sides_and_free_keeps_the_rests(self):
        for seed in range(3):
            p, t = sphere(seed)
            owners = (p[t].mean(axis=1)[:, 0] > 0).astype(int)
            border = np.intersect1d(np.unique(t[owners == 0]), np.unique(t[owners == 1]))
            results = {}
            for seam in ('free', 'lock'):
                allocation = Allocation(tri_o=owners, object_names=('left', 'right'), material_names=('plain',),
                                        meshes=({'match': {'object': 'left'}, 'seam': seam},))
                q, _, out, labels = simplify(p, np.full_like(p, 100), t, owners, len(t),
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
                                meshes=({'match': {'object': 'vase*'}},))
        with watched_simplify() as run:
            simplify(points, np.full_like(points, 100), faces, np.zeros(len(faces), dtype=int), len(faces),
                     colour_deviation=1, allocation=allocation)
        shared = next(call for call in run.call_args_list if len(call.args[0]) == len(p))
        centre = (p.min(axis=0) + p.max(axis=0)) / 2
        _, mapping = cKDTree(p).query(shared.args[0] + centre)
        np.testing.assert_array_equal(shared.kwargs['vertex_lock'], expected[mapping])
        # Only the first placement touches; the isolated copy shares its local locks.
        with watched_simplify() as run:
            simplify(points, np.full_like(points, 100), faces[:-1], np.zeros(len(faces) - 1, dtype=int), len(faces),
                     colour_deviation=1, allocation=Allocation(tri_o=owners[:-1], object_names=allocation.object_names,
                                                              material_names=allocation.material_names, meshes=allocation.meshes))
        shared = [call for call in run.call_args_list if len(call.args[0]) == len(p)]
        self.assertEqual(len(shared), 1)
        self.assertTrue(np.any(shared[0].kwargs['vertex_lock']))

    def test_weld_pins_vertex_to_face_contact(self):
        p = np.array([[0., 0, 0], [4, 0, 0], [0, 4, 0], [1, 1, 0], [1, 1, 2], [2, 1, 2]])
        t = np.arange(6).reshape(-1, 3)
        allocation = Allocation(tri_o=np.arange(2), object_names=('floor', 'column'), material_names=('plain',),
                                meshes=({'match': {'object': 'column'}, 'seam': 'weld'},))
        with watched_simplify() as run:
            simplify(p, np.full_like(p, 100), t, np.zeros(len(t), dtype=int), len(t), colour_deviation=1, allocation=allocation)
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[3], 2)
        np.testing.assert_array_equal(run.call_args.kwargs['vertex_lock'], [True, True, True, True, False, False])

    @needs_llvm
    def test_shared_bake_uses_albedo_then_placed_light(self):
        from r3d import mesh_import
        p, t = sphere(9)
        points = np.concatenate([p, p * [-1, 1, 1] + [6, 0, 0]])
        faces = np.concatenate([t, t[:, ::-1] + len(p)])
        src = SimpleNamespace(p=points, tri_v=faces, tri_t=None, uv=None, tri_m=np.zeros(len(faces), dtype=int),
                              tri_o=np.repeat([0, 1], len(t)), names=['plain'], object_names=['vase_a', 'vase_b'],
                              textures=[None], materials={}, colors=None)
        settings = SimpleNamespace(seed=0, position_scale=None, alpha_keep=None, thin=None, double_sided=set(), cards=None,
                     meshes=[{'match': {'object': 'vase*'}}],
                     simplify=SimpleNamespace(dense_edge=10, seal_seams=False, colour_deviation=1))
        job = SimpleNamespace(settings=settings, renderer=SimpleNamespace(visibility=None, variant=SimpleNamespace(name="mesh", triangles=len(faces))),
                              bake=SimpleNamespace(indirect=None, ray_offset=0.01, ao=None, colour_merge_step=6))
        scene = SimpleNamespace(lights=[{'type': 'directional', 'mode': LIGHT_MODE_BAKED, 'direction': [1, 0, 0], 'color': [1, 1, 1], 'intensity': 1}],
                                tonemap_white=1, indirect=SimpleNamespace(intensity=1))
        query = Open()
        with patch.object(mesh_import, 'load_source', return_value=src), \
             patch.object(mesh_import, 'RayQuery', return_value=query) as rays, \
             watched_simplify() as run, \
             patch.object(mesh_import, 'shade_placed', wraps=mesh_import.shade_placed) as shade:
            result = mesh_import.bake_geometry(job, scene, carve=True)
        self.assertEqual(run.call_count, 1)
        np.testing.assert_array_equal(run.call_args.args[1], np.full_like(run.call_args.args[0], 255))
        self.assertEqual(shade.call_count, 2)
        self.assertIs(shade.call_args_list[0].args[-2], query)
        np.testing.assert_array_equal(rays.call_args.args[1], faces)
        left, right = [lit for _, lit in result.shapes[0]["copies"]]
        self.assertFalse(np.array_equal(np.sort(left[1], axis=0), np.sort(right[1], axis=0)))
        self.assertEqual(result.mesh_report["scene_total"], len(faces))

        settings.thin = SimpleNamespace(material='plain', keep=0.7)
        job.renderer.visibility = SimpleNamespace(source='camera_region')
        src.tri_v = np.concatenate([t, t[::-1, ::-1] + len(p)])
        visible = np.repeat([True, False], len(t))
        with patch.object(mesh_import, 'load_source', return_value=src), \
             patch.object(mesh_import, 'RayQuery', return_value=query), \
             patch.object(mesh_import, 'visible_triangles', return_value=visible), \
             watched_simplify() as run:
            result = mesh_import.bake_geometry(job, scene, carve=True)
        self.assertEqual(run.call_count, 1)
        self.assertLess(len(run.call_args.args[2]), len(t))
        self.assertEqual(result.mesh_report['meshes'][0]['copies'], 2)
        self.assertGreater(result.shapes[0]["copies"][1][1][0][:, 0].mean(), 3)

    def test_shared_mirror_decimates_once_and_places_before_shading(self):
        p, t = sphere(8)
        reflected = p * [-1, 1, 1] + [6, 0, 0]
        points = np.concatenate([p, reflected])
        faces = np.concatenate([t, t[:, ::-1] + len(p)])
        owners = np.repeat([0, 1], len(t))
        shaded = []
        def shade(q, c, out, labels, **_):
            shaded.append(q.copy())
            return q, np.full_like(q, q[:, 0].mean()), out, labels
        allocation = Allocation(tri_o=owners, object_names=('vase_a', 'vase_b'), material_names=('plain',),
                                meshes=({'match': {'object': 'vase*'}},), shade=shade)
        with watched_simplify() as run:
            q, c, out, _ = simplify(points, np.full_like(points, 100), faces, np.zeros(len(faces), dtype=int), len(faces),
                                     colour_deviation=1, allocation=allocation)
        self.assertEqual(run.call_count, 1)
        self.assertEqual(len(shaded), 2)
        n = len(q) // 2
        np.testing.assert_allclose(q[:n] * [-1, 1, 1] + [6, 0, 0], q[n:], atol=1e-6)
        half = len(out) // 2
        np.testing.assert_array_equal(out[:half][:, ::-1] + n, out[half:])
        self.assertFalse(np.array_equal(c[:n], c[n:]))

    @staticmethod
    def _retriangulated_copies(shape, seam='free', touching=None, mirrored=False):
        """A vase and a turned copy of it split in four per triangle, in another material (`mirrored`: a reflected
        copy); with `touching` (0 the vase, 1 the copy), a rest triangle meets that one at one of its vertices.
        `shape` None leaves the rule without the key."""
        from scipy.spatial.transform import Rotation
        # Stretched and bent: a near-round or near-symmetric shape gives the surface match no start frame of its own.
        p, t = sphere(11)
        inward = np.einsum('ij,ij->i', np.cross(p[t[:, 1]] - p[t[:, 0]], p[t[:, 2]] - p[t[:, 0]]), p[t].mean(axis=1)) < 0
        t[inward] = t[inward][:, ::-1]  # wound outward, so a winding check means something
        p = bent(p)
        fine_p, fine_t = subdivided(p, t)
        turn = Rotation.from_rotvec([0.3, -0.8, 0.4]).as_matrix()
        if mirrored:
            turn, fine_t = turn @ np.diag([-1, 1, 1]), fine_t[:, ::-1]
        points = [p, fine_p @ turn.T + [6, 0, 0]]
        faces = [t, fine_t + len(p)]
        owners, labels = [np.zeros(len(t), int), np.ones(len(fine_t), int)], [np.zeros(len(t), int), np.ones(len(fine_t), int)]
        if touching is not None:
            corner = points[touching][0]
            points.append(np.array([corner, corner + [0, 0, 2], corner + [0, 2, 0]]))
            faces.append(np.array([[0, 1, 2]]) + len(p) + len(fine_p))
            owners.append([2])
            labels.append([0])
        allocation = Allocation(tri_o=np.concatenate(owners), object_names=('vase_a', 'vase_b', 'rest'),
                                material_names=('stone', 'brass'),
                                meshes=({'match': {'object': 'vase*'}, 'seam': seam,
                                         **({} if shape is None else {'shape': shape})},))
        return np.concatenate(points), np.concatenate(faces), np.concatenate(labels), allocation, (p, turn)

    def test_a_surface_rule_simplifies_a_retriangulated_copy_once_and_shades_it_in_its_own_material(self):
        points, faces, labels, allocation, (p, turn) = self._retriangulated_copies('surface')
        shaded = []

        def shade(q, c, out, labels, groups=None):
            shaded.append((labels.copy(), None if groups is None else groups.copy()))
            return q, c, out, labels
        allocation.shade, report = shade, {}
        allocation.report = report
        with watched_simplify() as run:
            q, _, out, out_labels = simplify(points, np.full_like(points, 100), faces, labels, len(faces),
                                             colour_deviation=1, allocation=allocation)
        self.assertEqual(run.call_count, 1)
        [entry] = report['meshes']
        self.assertEqual((entry['copies'], entry['joined']), (2, 1))
        n = len(q) // 2
        # The copy's stamp is the vase's, placed through the copy's own turn and move, in its material.
        np.testing.assert_allclose(q[n:], q[:n] @ turn.T + [6, 0, 0], atol=1e-5)
        np.testing.assert_array_equal(out_labels, np.repeat([0, 1], len(out) // 2))
        # The copy is lit in its own material, grouped by the shape's, so its corners keep the shape's order.
        self.assertEqual([labels.max() for labels, _ in shaded], [0, 1])
        for _, groups in shaded:
            np.testing.assert_array_equal(groups, np.zeros(len(out) // 2))

    def test_a_mirrored_copy_joined_by_surface_keeps_its_outside_out(self):
        points, faces, labels, allocation, _ = self._retriangulated_copies('surface', mirrored=True)
        report = {}
        allocation.report = report
        with watched_simplify() as run:
            q, _, out, _ = simplify(points, np.full_like(points, 100), faces, labels, len(faces), colour_deviation=1,
                                    allocation=allocation)
        self.assertEqual((run.call_count, report['meshes'][0]['joined']), (1, 1))
        half = len(out) // 2

        def normals(v, f):
            n = np.cross(v[f[:, 1]] - v[f[:, 0]], v[f[:, 2]] - v[f[:, 0]])
            return n / np.linalg.norm(n, axis=1)[:, None]
        # Every stamped triangle faces the way the source triangle nearest it does.
        for stamp, source in ((out[:half], faces[labels == 0]), (out[half:], faces[labels == 1])):
            near = nearest_source(points, source, q, stamp)
            self.assertGreater(np.einsum('ij,ij->i', normals(q, stamp), normals(points, source[near])).min(), 0)

    def test_an_exact_rule_keeps_a_retriangulated_copy_apart(self):
        # A rule without `shape` is exact.
        for shape in ('exact', None):
            with self.subTest(shape=shape):
                points, faces, labels, allocation, _ = self._retriangulated_copies(shape)
                report = {}
                allocation.report = report
                with watched_simplify() as run:
                    simplify(points, np.full_like(points, 100), faces, labels, len(faces), colour_deviation=1,
                             allocation=allocation)
                self.assertEqual(run.call_count, 2)
                self.assertFalse(any(row.get('joined') for row in report['meshes']))

    def test_a_copy_with_a_locked_border_is_not_joined_by_surface(self):
        # Its border vertices are not the shape's, so a stamp would open the seam.
        for seam, touching in (('lock', 1), ('weld', 1), ('lock', 0)):
            with self.subTest(seam=seam, touching=touching):
                points, faces, labels, allocation, _ = self._retriangulated_copies('surface', seam, touching)
                report = {}
                allocation.report = report
                with watched_simplify():
                    simplify(points, np.full_like(points, 100), faces, labels, len(faces), colour_deviation=1,
                             allocation=allocation)
                self.assertFalse(any(row.get('joined') for row in report['meshes']))

    def test_a_joined_copy_takes_for_each_shape_material_the_own_material_nearest_most_of_its_triangles(self):
        from r3d.simplify import own_materials
        p, t = sphere(10)
        fine_p, fine_t = subdivided(p, t)
        shape_materials = (p[t].mean(axis=1)[:, 0] > 0).astype(int)
        x = fine_p[fine_t].mean(axis=1)[:, 0]
        # Most of shape material 1 (x > 0) lies over own material 5, though 5 begins past x = 0.2.
        for own, expected in ((np.where(x > 0.2, 5, 7), [7, 5]), (np.full(len(fine_t), 9), [9, 9])):
            with self.subTest(expected=expected):
                found = own_materials(p, t, shape_materials, fine_p, fine_t, own)
                np.testing.assert_array_equal(found, np.array(expected)[shape_materials])

    def test_shading_runs_per_group_in_group_order_lit_as_each_groups_material(self):
        from r3d import mesh_import
        lit = []

        def shade_lit(src, job, scene, material, p, t, double, intersector, bounce, labels, preserve_corners):
            lit.append((material, len(t)))
            return p, np.zeros_like(p), t, labels
        src = SimpleNamespace(names=['stone', 'brass', 'cloth'])
        job = SimpleNamespace(settings=SimpleNamespace(double_sided=set()))
        positions = np.arange(18, dtype=float).reshape(6, 3)
        tris = np.array([[0, 1, 2], [3, 4, 5], [0, 2, 4]])
        labels = np.array([2, 0, 2])
        with patch.object(mesh_import, 'shade_lit', side_effect=shade_lit):
            *_, by_groups = mesh_import.shade_placed(src, job, None, positions, None, tris, labels, None, None,
                                                     groups=np.array([0, 1, 0]))
            self.assertEqual(lit, [(2, 2), (0, 1)])
            np.testing.assert_array_equal(by_groups, [2, 2, 0])
            lit.clear()
            *_, by_labels = mesh_import.shade_placed(src, job, None, positions, None, tris, labels, None, None)
            self.assertEqual(lit, [(0, 1), (2, 2)])
            np.testing.assert_array_equal(by_labels, [0, 2, 2])


if __name__ == '__main__':
    unittest.main()
