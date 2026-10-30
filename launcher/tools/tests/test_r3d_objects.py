"""Synthetic source objects, rigid placements and offline scene reporting."""

import csv
from dataclasses import replace
from itertools import product
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np
from scipy.spatial.transform import Rotation

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from gltf.gltf_write import build_glb
from r3d.gltf_mesh import load_gltf_mesh
from r3d.obj import Texture, load_obj
from r3d.import_settings import SettingsError, load_import_settings, load_scene
from r3d.objects.instances import MATCH_TOLERANCE, SourceObject, bounds_diagonal, match_instances, rigid_fit, source_objects
from r3d.objects.objects_report import LOD_LEVELS, LOD_TRIANGLE_RATIO, main, masked_objects, pose_rows
from r3d.mesh_import import load_source
from test_r3d_import import camera, renderer, write_import, write_scene

POINTS = np.array([[0., 0., 0.], [3., 0., 0.], [0., 2., 0.], [0., 0., 1.]])
TRIANGLES = np.array([[0, 2, 1], [0, 1, 3], [0, 3, 2], [1, 2, 3]])
MATERIALS = np.array([0, 1, 0, 1])


def obj_text(copies):
    lines = []
    offset = 0
    for name, points, triangles in copies:
        lines.append(f'o {name}')
        lines.extend('v ' + ' '.join(map(str, point)) for point in points)
        for face, material in zip(triangles, MATERIALS):
            lines.extend([f'usemtl material_{material}', 'f ' + ' '.join(str(int(v) + offset + 1) for v in face)])
        offset += len(points)
    return '\n'.join(lines) + '\n'


def pair_shapes(points=POINTS, triangles=TRIANGLES, materials=MATERIALS, tolerance=MATCH_TOLERANCE):
    return match_instances([SourceObject('original', POINTS, TRIANGLES, MATERIALS),
                            SourceObject('copy', points, triangles, materials)], tolerance)


class ObjectsTests(unittest.TestCase):
    def test_obj_rotated_instance(self):
        rotation = Rotation.from_rotvec(np.array([1., 2., 3.]) * 0.37).as_matrix()
        shapes = pair_shapes(POINTS @ rotation.T + [7, 4, 9])
        self.assertEqual(len(shapes), 1)
        self.assertFalse(shapes[0].instances[1].mirrored)
        placed = shapes[0].positions @ shapes[0].instances[1].linear.T + shapes[0].instances[1].position
        np.testing.assert_allclose(placed, POINTS @ rotation.T + [7, 4, 9])
        again = pair_shapes(POINTS @ rotation.T + [7, 4, 9])
        for first, second in zip(shapes[0].instances, again[0].instances):
            np.testing.assert_array_equal(first.linear, second.linear)
            np.testing.assert_array_equal(first.position, second.position)

    def test_obj_mirrored_instance(self):
        shapes = pair_shapes(POINTS * [-1, 1, 1], TRIANGLES[:, ::-1])
        self.assertEqual(len(shapes), 1)
        self.assertTrue(shapes[0].instances[1].mirrored)

    def test_obj_reordered_instance(self):
        permutation = np.array([2, 0, 3, 1])
        shapes = pair_shapes(POINTS[permutation], np.roll(np.argsort(permutation)[TRIANGLES[::-1]], 1, axis=1),
                             MATERIALS[::-1])
        self.assertEqual(len(shapes), 1)
        self.assertFalse(shapes[0].instances[1].mirrored)
        self.assertEqual(shapes[0].instances[1].triangle_order, tuple(reversed(range(len(TRIANGLES)))))

    def test_obj_mirrored_reordered_instance(self):
        permutation = np.array([2, 0, 3, 1])
        shapes = pair_shapes((POINTS * [-1, 1, 1])[permutation], np.argsort(permutation)[TRIANGLES[:, ::-1]])
        self.assertEqual(len(shapes), 1)
        self.assertTrue(shapes[0].instances[1].mirrored)

    def test_symmetric_reflection_matches_as_rotation(self):
        points = np.array([[-2., 0., 0.], [2., 0., 0.], [0., 3., 0.], [0., 0., 1.]])
        for seed in range(4):
            rotation = Rotation.random(random_state=seed).as_matrix()
            placed = (points * [-1, 1, 1]) @ rotation.T + [7, 4, 9]
            for permutation, same_topology in ((np.arange(len(points)), False),
                                               (np.array([2, 0, 3, 1]), False),
                                               (np.arange(len(points)), True)):
                with self.subTest(seed=seed, permutation=permutation, same_topology=same_topology):
                    triangles = np.concatenate([TRIANGLES, TRIANGLES[:, ::-1]]) if same_topology else TRIANGLES
                    materials = np.zeros(len(triangles), dtype=int)
                    original = SourceObject('wedge', points, triangles, materials)
                    faces = triangles if same_topology else np.argsort(permutation)[triangles[:, ::-1]]
                    copy = SourceObject('copy', placed[permutation], faces, materials)
                    shapes = match_instances([original, copy])
                    self.assertEqual(len(shapes), 1)
                    self.assertFalse(shapes[0].instances[1].mirrored)

    def test_obj_scaled_copy_is_separate(self):
        self.assertEqual(len(pair_shapes(POINTS * 2)), 2)

    def test_obj_nonuniform_scaled_copy_is_separate(self):
        self.assertEqual(len(pair_shapes(POINTS * [2, 1, 1])), 2)

    def test_obj_distorted_copy_is_separate(self):
        points = POINTS.copy()
        points[1, 0] += 0.1
        self.assertEqual(len(pair_shapes(points)), 2)

    def test_planar_proper_rotations_are_not_mirrored(self):
        points = np.array([[0., 0., 0.], [3., 0., 0.], [2., 2., 0.], [0., 1., 0.]])
        faces = np.array([[0, 1, 2], [0, 2, 3]])
        original = SourceObject('quad', points, faces, np.zeros(2, dtype=int))
        for seed in range(4):
            for rotation in Rotation.random(50, random_state=seed).as_matrix():
                with self.subTest(seed=seed, rotation=rotation):
                    copy = SourceObject('copy', points @ rotation.T + [7, 4, 9], faces, original.materials)
                    shapes = match_instances([original, copy])
                    self.assertEqual(len(shapes), 1)
                    self.assertFalse(shapes[0].instances[1].mirrored)

    def test_planar_mirror_is_mirrored(self):
        points = np.array([[0., 0., 0.], [3., 0., 0.], [2., 2., 0.], [0., 1., 0.]])
        faces = np.array([[0, 1, 2], [0, 2, 3]])
        objects = [SourceObject('quad', points, faces, np.zeros(2, dtype=int)),
                   SourceObject('mirror', points * [-1, 1, 1], faces[:, ::-1], np.zeros(2, dtype=int))]
        shapes = match_instances(objects)
        self.assertEqual(len(shapes), 1)
        self.assertTrue(shapes[0].instances[1].mirrored)

    def test_inside_out_is_not_nonmirrored(self):
        self.assertEqual(len(pair_shapes(triangles=TRIANGLES[:, ::-1])), 2)

    def test_material_permuted_across_faces_is_separate(self):
        self.assertEqual(len(pair_shapes(materials=MATERIALS[::-1])), 2)

    def test_different_connectivity_is_separate(self):
        faces = TRIANGLES.copy()
        faces[0] = faces[1]
        self.assertEqual(len(pair_shapes(triangles=faces)), 2)

    def test_rigid_fit_of_solid_flat_and_line_copies(self):
        rng = np.random.default_rng(5)
        for shape, axes in (("solid", [3.0, 2.0, 1.0]), ("flat", [3.0, 2.0, 0.0]), ("line", [3.0, 0.0, 0.0])):
            local = rng.normal(size=(40, 3)) * axes
            for mirrored in (False, True):
                with self.subTest(shape=shape, mirrored=mirrored):
                    turn = Rotation.from_euler("xyz", [30, -50, 110], degrees=True).as_matrix()
                    placement = turn @ np.diag([-1.0, 1.0, 1.0]) if mirrored else turn
                    target = local @ placement.T + [4.0, -2.0, 7.0]
                    linear, _, residual = rigid_fit(local, target, reflected=mirrored)
                    self.assertAlmostEqual(np.linalg.det(linear), -1.0 if mirrored else 1.0, places=5)
                    self.assertLess(residual, 1e-5 * bounds_diagonal(target))

    def test_tolerance_boundary(self):
        points = POINTS.copy()
        points[1, 0] += 0.01
        residual = rigid_fit(POINTS, points)[2]
        relative = residual / bounds_diagonal(points)
        self.assertEqual(len(pair_shapes(points, tolerance=relative * 1.001)), 1)
        self.assertEqual(len(pair_shapes(points, tolerance=relative * 0.999)), 2)

    def test_zero_tolerance(self):
        points = POINTS.copy()
        points[1, 0] += 1e-10
        self.assertEqual(len(pair_shapes(tolerance=0)), 1)
        self.assertEqual(len(pair_shapes(points, tolerance=0)), 2)

    def test_invalid_tolerances(self):
        for value in (float('nan'), float('inf'), -1):
            with self.subTest(value=value), self.assertRaises(ValueError):
                pair_shapes(tolerance=value)

    def test_zero_diagonal_matches(self):
        obj = SourceObject('point', np.zeros_like(POINTS), TRIANGLES, MATERIALS)
        shapes = match_instances([obj, obj])
        self.assertEqual(len(shapes), 1)
        self.assertEqual(shapes[0].instances[1].residual, 0)

    def test_six_significant_digit_copy_matches_default(self):
        rotation = Rotation.from_rotvec(np.array([1., 2., 3.]) * 0.37).as_matrix()
        placed = POINTS @ rotation.T + [712, 942, 1092]
        rounded = np.array([[float(f'{value:.6g}') for value in point] for point in placed])
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'rounded.obj'
            path.write_text(obj_text([('original', POINTS, TRIANGLES), ('rounded', rounded, TRIANGLES)]))
            p, _, tv, _, tm, _, to, names = load_obj(path)
        shapes = match_instances(source_objects(p, tv, tm, to, names))
        self.assertEqual([len(shape.instances) for shape in shapes], [2])

    def test_density_lod_only_trims_repeated_shapes(self):
        points = np.concatenate([POINTS + [0, 0, -100], POINTS + [1, 0, -100],
                                 POINTS * [1.1, 1, 1] + [-1, 0, -100]])
        triangles = np.concatenate([TRIANGLES + i * len(POINTS) for i in range(3)])
        shapes = match_instances(source_objects(points, triangles, np.tile(MATERIALS, 3),
                                                np.repeat(np.arange(3), len(TRIANGLES)),
                                                ['first', 'copy', 'single']))
        job = SimpleNamespace(renderer=SimpleNamespace(variant=SimpleNamespace(triangles=None)),
                              object=SimpleNamespace(matrix=np.eye(3), position=np.zeros(3)))
        poses = np.array([[0., 0., 0., 0., 0., -1.], [0., 0., -95., 0., 0., -1.],
                          [0., 0., -99., 0., 0., -1.]])
        width, height = 184, 224
        with patch('r3d.objects.objects_report.sample_camera_path',
                   return_value=(width, height, 1., 0.1, poses)):
            component = SimpleNamespace(path=SimpleNamespace(animation=None, node=None),
                                        half_fov_short_tan=1., near_z=0.1)
            scene = SimpleNamespace(camera=SimpleNamespace(component=component))
            result = pose_rows(shapes, scene, job, 1000, (width, height))
        budget, rows = result[:2]
        self.assertEqual(budget, len(triangles))
        self.assertEqual(rows[0]['budget_triangles_in_view'], budget)
        self.assertAlmostEqual(rows[0]['triangles_with_lod'], len(TRIANGLES) * (1 + 2 / LOD_TRIANGLE_RATIO ** (LOD_LEVELS - 1)))
        self.assertEqual(rows[1]['triangles_with_lod'], len(TRIANGLES) * (1 + 2 / LOD_TRIANGLE_RATIO))
        self.assertEqual(rows[2]['triangles_with_lod'], budget)
        self.assertAlmostEqual(result[2], width * height / budget)
        for row in rows:
            self.assertEqual(row['budget_triangles'], budget)
            self.assertAlmostEqual(row['saved'], row['budget_triangles_in_view'] - row['triangles_with_lod'])

    def test_obj_groups_and_default(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'plain.obj'
            path.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\ng next\nf 1 3 2\n')
            loaded = load_obj(path)[-2:]
            self.assertEqual(loaded[1], ['plain', 'next'])
            np.testing.assert_array_equal(loaded[0], [0, 1])
            path.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n')
            self.assertEqual(load_obj(path)[-2:][1], ['plain'])

    def test_gltf_shared_and_baked(self):
        mesh = {'primitives': [{'positions': POINTS.tolist(), 'indices': TRIANGLES.ravel().tolist()}]}
        baked = {'primitives': [{'positions': (POINTS + [8, 2, 1]).tolist(), 'indices': TRIANGLES.ravel().tolist()}]}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'source.glb'
            path.write_bytes(build_glb([{'mesh': 0, 'name': 'first'}, {'mesh': 0, 'translation': [4, 0, 0]},
                                        {'mesh': 1}], [], meshes=[mesh, baked]))
            src = load_gltf_mesh(path)
            self.assertEqual(len(src.tri_o), len(src.tri_v))
        objects = source_objects(src.positions, src.tri_v, src.tri_m, src.tri_o, src.object_names)
        self.assertEqual(src.object_names, ['first', 'node_1', 'node_2'])
        shapes = match_instances(objects)
        self.assertEqual((len(shapes), len(shapes[0].instances)), (1, 3))

    def test_unhydrated_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write_import(directory)
            settings = load_import_settings(path)
            settings.source['path'].write_text('version https://git-lfs.github.com/spec/v1\n'
                                               + 'oid sha256:' + hashlib.sha256(b'').hexdigest() + '\nsize 1\n')
            with self.assertRaisesRegex(SettingsError, 'Git LFS'):
                load_source(settings)

    def alpha_source(self, directory):
        from PIL import Image

        path = pathlib.Path(directory) / 'alpha.png'
        Image.fromarray(np.array([[[255, 255, 255, 0], [255, 255, 255, 255]]], dtype=np.uint8)).save(path)
        triangles = np.concatenate([TRIANGLES, TRIANGLES + len(POINTS)])
        return SimpleNamespace(p=np.concatenate([POINTS, POINTS + [10, 0, 0]]),
                               uv=np.array([[0.25, 0.5], [0.75, 0.5]]), tri_v=triangles,
                               tri_t=np.ones_like(triangles), tri_m=np.zeros(len(triangles), dtype=int),
                               tri_o=np.repeat([0, 1], len(TRIANGLES)), object_names=['first', 'copy'],
                               textures=[Texture(path)])

    def test_alpha_object_disappears(self):
        with tempfile.TemporaryDirectory() as directory:
            src = self.alpha_source(directory)
            src.tri_t[len(TRIANGLES):] = 0
            objects, triangles = masked_objects(src, SimpleNamespace(alpha_keep=0.5))
        self.assertEqual([obj.name for obj in objects], ['first'])
        np.testing.assert_array_equal(triangles, src.tri_v[:len(TRIANGLES)])

    def test_alpha_partial_copy_no_longer_matches(self):
        with tempfile.TemporaryDirectory() as directory:
            src = self.alpha_source(directory)
            src.tri_t[-1] = 0
            objects, triangles = masked_objects(src, SimpleNamespace(alpha_keep=0.5))
        self.assertEqual([len(obj.triangles) for obj in objects], [4, 3])
        np.testing.assert_array_equal(triangles, src.tri_v[:-1])
        self.assertEqual(len(match_instances(objects)), 2)

    def test_alpha_none_keeps_everything(self):
        with tempfile.TemporaryDirectory() as directory:
            src = self.alpha_source(directory)
            src.tri_t[:] = 0
            objects, triangles = masked_objects(src, SimpleNamespace(alpha_keep=None))
        self.assertEqual([len(obj.triangles) for obj in objects], [4, 4])
        np.testing.assert_array_equal(triangles, src.tri_v)
        self.assertEqual(len(match_instances(objects)), 1)

    def test_load_obj_source_alignment(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory))
            settings.source['path'].write_text(obj_text([('first', POINTS, TRIANGLES),
                                                        ('second', POINTS + 10, TRIANGLES)]))
            src = load_source(settings)
        self.assertEqual(src.object_names, ['first', 'second'])
        np.testing.assert_array_equal(src.tri_o, np.repeat([0, 1], 4))
        np.testing.assert_array_equal(src.p[src.tri_v[src.tri_o == 1]], (POINTS + 10)[TRIANGLES])

    def test_load_gltf_source_alignment(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory))
            path = pathlib.Path(directory) / 'source.glb'
            mesh = {'primitives': [{'positions': POINTS.tolist(), 'indices': TRIANGLES.ravel().tolist()}]}
            path.write_bytes(build_glb([{'mesh': 0, 'name': 'first'}, {'mesh': 0, 'translation': [10, 0, 0]}],
                                       [], meshes=[mesh]))
            settings.source['path'] = path
            src = load_source(settings)
        self.assertEqual(src.object_names, ['first', 'node_1'])
        np.testing.assert_array_equal(src.tri_o, np.repeat([0, 1], 4))
        np.testing.assert_array_equal(src.p[src.tri_v[src.tri_o == 1]], (POINTS + [10, 0, 0])[TRIANGLES])

    def run_pose(self, objects, budget=None, matrix=None, position=None):
        job = SimpleNamespace(renderer=SimpleNamespace(variant=SimpleNamespace(triangles=budget)),
                              object=SimpleNamespace(matrix=np.eye(3) if matrix is None else matrix,
                                                     position=np.zeros(3) if position is None else position))
        with patch('r3d.objects.objects_report.sample_camera_path',
                   return_value=(184, 224, 1., 0.1, np.array([[0., 0., 0., 0., 0., -1.]]))):
            component = SimpleNamespace(path=SimpleNamespace(animation=None, node=None),
                                        half_fov_short_tan=1., near_z=0.1)
            scene = SimpleNamespace(camera=SimpleNamespace(component=component))
            return pose_rows(match_instances(objects), scene, job, 1000, (184, 224))

    def test_pose_explicit_budget_proportional_shares(self):
        objects = [SourceObject('inside', POINTS + [0, 0, -10], TRIANGLES[:1], MATERIALS[:1]),
                   SourceObject('outside', POINTS + [1000, 0, -10], TRIANGLES, MATERIALS)]
        budget, rows, _ = self.run_pose(objects, budget=100)
        self.assertEqual(budget, 100)
        self.assertEqual(rows[0]['budget_triangles_in_view'], 20)
        self.assertEqual(rows[0]['triangles_with_lod'], 20)

    def test_pose_renderer_matrix_and_position(self):
        obj = SourceObject('placed', POINTS + [100, 0, 0], TRIANGLES, MATERIALS)
        matrix = Rotation.from_euler('y', 90, degrees=True).as_matrix()
        _, rows, _ = self.run_pose([obj], matrix=matrix, position=[0, 0, 90])
        self.assertEqual(rows[0]['instances_in_view'], 1)
        _, rows, _ = self.run_pose([obj])
        self.assertEqual(rows[0]['instances_in_view'], 0)

    def test_pose_behind_camera_and_zero_mean(self):
        obj = SourceObject('behind', POINTS + [0, 0, 10], TRIANGLES, MATERIALS)
        _, rows, density = self.run_pose([obj])
        self.assertEqual(rows[0]['instances_in_view'], 0)
        self.assertEqual(rows[0]['triangles_with_lod'], 0)
        self.assertEqual(density, float('inf'))

    def test_zero_diagonal_report(self):
        from r3d.objects.objects_report import write_report

        obj = SourceObject('point', np.zeros_like(POINTS), TRIANGLES, MATERIALS)
        shapes = match_instances([obj, obj])
        budget, rows, density = self.run_pose([obj, obj], position=[0, 0, -10])
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory)
            write_report(out, [obj, obj], shapes, budget, rows, density, MATCH_TOLERANCE)
            self.assertTrue((out / 'residuals.png').is_file())
            self.assertTrue(np.isfinite(rows[0]['triangles_with_lod']))

    def test_cli_tolerance_reaches_matching(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_import(root)
            scene = write_scene(root, renderer() + camera(path=True, region=False))
            with patch('r3d.objects.objects_report.match_instances', wraps=match_instances) as matching:
                main([str(scene), '--object', 'mesh', '--out', str(root / 'out'), '--size', '184', '224',
                      '--tolerance', '0.0123'])
            self.assertEqual(matching.call_args.args[1], 0.0123)

    def test_cli_selected_renderer_size(self):
        for fit_size, visibility_size, expected in (((92, 112), (8, 6), (92, 112)),
                                                    (None, (8, 6), (8, 6)), (None, None, None)):
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                write_import(root)
                path = write_scene(root, renderer(name='other') + renderer() + camera(path=True, region=False))
                scene = load_scene(path)
                scene.renderers[0].renderer.fit = SimpleNamespace(size=(184, 224))
                scene.renderers[1].renderer.fit = SimpleNamespace(size=fit_size) if fit_size else None
                scene.renderers[1].renderer.visibility = (SimpleNamespace(size=visibility_size, source='camera_path',
                                                                          every_ms=1000) if visibility_size else None)
                with patch('r3d.objects.objects_report.load_scene', return_value=scene), \
                        patch('r3d.objects.objects_report.pose_rows', wraps=pose_rows) as report:
                    args = [str(path), '--object', 'mesh', '--out', str(root / 'out')]
                    if expected is None:
                        with self.assertRaises(SystemExit):
                            main(args)
                        report.assert_not_called()
                    else:
                        main(args)
                        self.assertEqual(tuple(report.call_args.args[-1]), expected)

    def test_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_import(root)
            (root / 'm.obj').write_text(obj_text([('inside', POINTS + [0, 0, -10], TRIANGLES),
                                                 ('outside', POINTS + [10000, 0, -10], TRIANGLES)]))
            scene = write_scene(root, renderer() + camera(path=True, region=False))
            out = root / 'report'
            main([str(scene), '--object', 'mesh', '--out', str(out), '--every-ms', '1000', '--size', '184', '224'])
            for name in ('objects.json', 'objects.md', 'residuals.png', 'poses.csv', 'lod.png'):
                self.assertTrue((out / name).is_file(), name)
            data = json.loads((out / 'objects.json').read_text())
            self.assertEqual(data['totals']['objects'], 2)
            self.assertEqual(data['totals']['shapes'], 1)
            self.assertEqual(data['totals']['source_triangles'], 8)
            self.assertEqual(data['totals']['unique_triangles'], 4)
            self.assertEqual(data['totals']['repeated_shapes'], 1)
            with (out / 'poses.csv').open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertTrue(rows)
            self.assertEqual(int(rows[0]['instances_in_view']), 1)
            self.assertEqual(float(rows[0]['budget_triangles_in_view']), 4)
            self.assertEqual(float(rows[0]['triangles_with_lod']), 4 / LOD_TRIANGLE_RATIO ** 2)
            self.assertEqual(float(rows[0]['budget_triangles']), 8)
            self.assertEqual(float(rows[0]['saved']), 4 - float(rows[0]['triangles_with_lod']))
            summary = (out / 'objects.md').read_text()
            self.assertIn('Calibrated pixels per triangle:', summary)
            self.assertIn('| Triangles LOD frees |', summary)


class InstancesFigureTests(unittest.TestCase):
    def test_view_visible_length_then_shapes_copies_earliest(self):
        from r3d.objects.instances_figure import choose_view

        boxes = np.array([np.array(list(product((-1., 1.), repeat=3))) + [0, 0, -10]] * 4)
        owners = np.array([0, 0, 1, 1])
        poses = [np.array([index, 0., 0., 0., 0., -1.]) for index in range(4)]
        segments = np.array([[[0., 0.], [10., 0.]], [[0., 0.], [2., 0.]]])
        samples = [(segments / 2, owners[:2]), (segments, owners[:2]),
                   (segments, owners[1:3]), (segments, owners[1:3])]
        with patch('r3d.objects.instances_figure.visible_segments', side_effect=samples), \
                patch('r3d.objects.instances_figure.clusters_in_view', side_effect=map(np.array,
                      [[True, True, True, True], [True, True, False, False],
                       [True, True, True, False], [True, True, True, False]])):
            index, _, _, _, _ = choose_view(boxes, owners, poses, (100, 80), 0.6, 1., None)
        self.assertEqual(index, 2)
        with patch('r3d.objects.instances_figure.visible_segments', side_effect=samples[:2]):
            self.assertEqual(choose_view(boxes, owners, poses[:2], (100, 80), 0.6, 1., None)[0], 1)

    def test_projected_corner_matches_camera_ray(self):
        from r3d.objects.instances_figure import project_points
        from r3d.poses import camera_rays

        size, lens = (20, 16), 0.6
        pose = np.array([3., 7., 2., -1., -0.2, -3.])
        origins, rays = camera_rays(*size, lens, pose[:3], pose[3:], 1)
        for pixel in (0, 83, 319):
            corner = origins[pixel] + rays[pixel] * 12
            projected, depth = project_points(np.array([corner]), pose, size, lens, 1.)
            np.testing.assert_allclose(projected[0], [pixel % size[0] + 0.5, pixel // size[0] + 0.5])
            self.assertGreater(depth[0], 1.)

    def test_edge_occlusion_wall(self):
        from r3d.objects.instances_figure import edge_visibility
        from r3d.ray_query import RayQuery

        wall = np.array([[-5., -5., -5.], [5., -5., -5.], [5., 5., -5.], [-5., 5., -5.]])
        query = RayQuery(wall, np.array([[0, 1, 2], [0, 2, 3]]), variant='scalar_rgb')
        for z, expected in ((-10., False), (-2., True)):
            _, visible = edge_visibility(query, np.zeros(3), np.array([-1., 0., z]), np.array([1., 0., z]), 0.001)
            self.assertTrue((visible == expected).all())

    def test_visible_segments_near_plane(self):
        from r3d.objects.instances_figure import edge_visibility, project_points, visible_segments
        from r3d.ray_query import RayQuery

        pose, size, lens, near = np.array([0., 0., 0., 0., 0., -1.]), (100, 80), 0.6, 1.
        edge = np.array([[0.2, 0.1, -0.5], [0.2, 0.1, -2.]])
        boxes = np.tile(edge, (1, 4, 1))
        query = RayQuery(POINTS + [1000, 0, 0], TRIANGLES, variant='scalar_rgb')
        with patch('r3d.objects.instances_figure.BOX_EDGES', ((0, 1),)), \
                patch('r3d.objects.instances_figure.edge_visibility', wraps=edge_visibility) as sampled:
            segments, owners = visible_segments(boxes, np.array([7]), pose, size, lens, near, query)
        np.testing.assert_allclose(sampled.call_args.args[2], [[0.2, 0.1, -near]])
        expected, _ = project_points(np.array([[0.2, 0.1, -near], edge[1]]), pose, size, lens, near)
        np.testing.assert_allclose(segments[0, 0], expected[0])
        np.testing.assert_allclose(segments[-1, 1], expected[1])
        self.assertTrue((owners == 7).all())

    def test_visible_segments_picture_border(self):
        from r3d.objects.instances_figure import visible_segments
        from r3d.ray_query import RayQuery

        size = (100, 80)
        boxes = np.tile(np.array([[0., 0., -2.], [10., 0., -2.]]), (1, 4, 1))
        query = RayQuery(POINTS + [1000, 0, 0], TRIANGLES, variant='scalar_rgb')
        with patch('r3d.objects.instances_figure.BOX_EDGES', ((0, 1),)):
            segments, owners = visible_segments(boxes, np.array([3]), np.array([0., 0., 0., 0., 0., -1.]),
                                               size, 0.6, 1., query)
        np.testing.assert_allclose(segments[0, 0], np.array(size) / 2)
        np.testing.assert_allclose(segments[-1, 1], [size[0], size[1] / 2])
        self.assertTrue((segments >= 0).all())
        self.assertTrue((segments <= size).all())
        self.assertTrue((owners == 3).all())

    def test_visible_segments_offscreen_and_behind_eye(self):
        from r3d.objects.instances_figure import visible_segments
        from r3d.ray_query import RayQuery

        query = RayQuery(POINTS + [1000, 0, 0], TRIANGLES, variant='scalar_rgb')
        for offset in ([100., 0., -5.], [0., 0., 5.]):
            with self.subTest(offset=offset):
                boxes = np.array([np.array(list(product((-1., 1.), repeat=3))) + offset])
                segments, owners = visible_segments(boxes, np.array([9]), np.array([0., 0., 0., 0., 0., -1.]),
                                                   (100, 80), 0.6, 1., query)
                self.assertEqual(segments.shape, (0, 2, 2))
                self.assertEqual(owners.shape, (0,))

    def test_visible_segments_owners_after_culling(self):
        from r3d.objects.instances_figure import visible_segments
        from r3d.ray_query import RayQuery

        boxes = np.array([np.array(list(product((-1., 1.), repeat=3))) + offset
                          for offset in ([100., 0., -5.], [-2., 0., -10.], [0., 0., 5.], [2., 0., -10.])])
        query = RayQuery(POINTS + [1000, 0, 0], TRIANGLES, variant='scalar_rgb')
        segments, owners = visible_segments(boxes, np.array([11, 23, 37, 49]),
                                           np.array([0., 0., 0., 0., 0., -1.]), (100, 80), 0.6, 1., query)
        self.assertEqual(set(owners), {23, 49})
        self.assertTrue((segments[owners == 23, :, 0] < 50).all())
        self.assertTrue((segments[owners == 49, :, 0] > 50).all())

    def test_draw_figure_selected_owner_inside_picture(self):
        from PIL import Image
        from r3d.objects.instances_figure import draw_figure

        picture = Image.new('RGB', (100, 80), 'white')
        segments = np.array([[[10., 20.], [30., 20.]], [[60., 20.], [80., 20.]]])
        colour = (200, 30, 50)
        image = draw_figure(picture, segments, np.array([7, 9]), {7: colour}, [])
        offset = 0
        self.assertEqual(image.getpixel((offset + 20, 20)), colour)
        self.assertEqual(image.getpixel((offset + 70, 20)), (255, 255, 255))

    def test_material_legend_top_lengths_suffixes_and_overflow(self):
        from r3d.objects.instances_figure import legend_rows

        shape = pair_shapes(POINTS + 10)[0]
        shapes = [shape, replace(shape, triangles=np.tile(shape.triangles, (2, 1))), shape]
        colours, rows = legend_rows(shapes, {0: 2., 1: 8., 2: 4.}, ['bronze', 'stone'],
                                    palette=('red', 'blue'))
        self.assertEqual(rows, [('red', 'bronze, 8 tris', 2), ('blue', 'bronze, 4 tris', 2),
                                (None, '1 more repeated shapes not drawn', None)])
        self.assertEqual(colours, {1: 'red', 2: 'blue'})

    def test_material_legend_unique_name_has_triangle_count(self):
        from r3d.objects.instances_figure import legend_rows

        shape = pair_shapes(POINTS + 10)[0]
        _, rows = legend_rows([shape], {0: 1.}, ['bronze', 'stone'])
        self.assertEqual(rows, [(rows[0][0], 'bronze, 4 tris', 2)])

    def test_legend_column_follows_rows_and_stays_right_of_picture(self):
        from PIL import Image, ImageDraw
        from r3d.objects.instances_figure import PADDING, ROW_HEIGHT, draw_figure
        from render.render_compare import _font

        picture = Image.new('RGB', (100, 80), 'black')
        colour = (200, 30, 50)
        overflow = '1 more repeated shapes not drawn'
        font = _font()
        text = ImageDraw.ImageDraw.text
        for name, count in [('bronze', 1), ('long material name', 6)]:
            with self.subTest(name=name):
                label = f'{name}, 4 tris, 2 copies'
                rows = [(colour, f'{name}, 4 tris', 2)] * count + [(None, overflow, None)]
                measure = ImageDraw.Draw(picture)
                width = max(measure.textbbox((0, 0), label, font=font)[2] + ROW_HEIGHT,
                            measure.textbbox((0, 0), overflow, font=font)[2]) + PADDING * 2
                with patch.object(ImageDraw.ImageDraw, 'text', autospec=True, side_effect=text) as drawn:
                    image = draw_figure(picture, [], [], {}, rows)
                self.assertEqual(image.size, (picture.width + width,
                                             max(picture.height, len(rows) * ROW_HEIGHT + PADDING * 2)))
                self.assertEqual(image.crop((0, 0, *picture.size)).tobytes(), picture.tobytes())
                self.assertEqual(image.getpixel((picture.width + PADDING, PADDING)), colour)
                self.assertEqual([call.args[2] for call in drawn.call_args_list], [label] * count + [overflow])
                self.assertTrue(all(call.args[1][0] >= picture.width + PADDING for call in drawn.call_args_list))

    def test_generate_with_blank_host_image(self):
        from PIL import Image
        from r3d.objects.instances_figure import DOC_SIZE, generate

        src = SimpleNamespace(p=np.concatenate([POINTS, POINTS + [5, 0, 0]]),
                              tri_v=np.concatenate([TRIANGLES, TRIANGLES + len(POINTS)]),
                              tri_m=np.tile(MATERIALS, 2), tri_o=np.repeat([0, 1], len(TRIANGLES)),
                              object_names=['first', 'second'], names=['bronze', 'stone'])
        job = SimpleNamespace(settings=SimpleNamespace(alpha_keep=None),
                              object=SimpleNamespace(matrix=np.eye(3), position=np.zeros(3), name='mesh'))
        scene = SimpleNamespace(path=pathlib.Path('test.scene.toml'))
        component = SimpleNamespace(path=SimpleNamespace(animation='unused', node='camera'),
                                    half_fov_short_tan=0.6, near_z=1.)
        cameras = [SimpleNamespace(name=name, component=component) for name in ('far', 'near')]
        pose = np.array([2., 3., 20., 0., 0., -1.])
        calls = []

        def blank(*args):
            calls.append(args)
            return Image.new('RGB', DOC_SIZE, 'white')

        with tempfile.TemporaryDirectory() as directory, \
                patch('r3d.objects.instances_figure.load_source', return_value=src), \
                patch('r3d.objects.instances_figure.sample_camera_path', side_effect=[
                    (*DOC_SIZE, 0.6, 1., [pose + [0, 0, 20, 0, 0, 0]]),
                    (*DOC_SIZE, 0.6, 1., [pose + [0, 0, 20, 0, 0, 0], pose])]):
            out = pathlib.Path(directory) / 'figure.png'
            generate(scene, job, cameras, pathlib.Path('unused'), out, 1000, host_render=blank)
            self.assertTrue(out.exists())
            with Image.open(out) as image:
                self.assertEqual(image.height, DOC_SIZE[1])
                self.assertGreater(image.width, DOC_SIZE[0])
                self.assertGreater(len(image.getcolors(image.width * image.height)), 1)
        self.assertEqual(calls[0][1:], ('test', 'mesh', 'near', 1000))


if __name__ == '__main__':
    unittest.main()
