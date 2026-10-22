"""Synthetic source objects, rigid placements and offline scene reporting."""

import csv
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
from r3d.objects.source import obj_ownership, gltf_ownership, load_object_source
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

    def test_obj_mirrored_reordered_instance(self):
        permutation = np.array([2, 0, 3, 1])
        shapes = pair_shapes((POINTS * [-1, 1, 1])[permutation], np.argsort(permutation)[TRIANGLES[:, ::-1]])
        self.assertEqual(len(shapes), 1)
        self.assertTrue(shapes[0].instances[1].mirrored)

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
            p, _, tv, _, tm, _ = load_obj(path)
            to, names = obj_ownership(path)
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
        with patch('r3d.objects.objects_report.camera_path_poses',
                   return_value=(width, height, 1., 0.1, poses)):
            result = pose_rows(shapes, None, job, 1000, (width, height))
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
            loaded = obj_ownership(path)
            self.assertEqual(loaded[1], ['plain', 'next'])
            np.testing.assert_array_equal(loaded[0], [0, 1])
            path.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n')
            self.assertEqual(obj_ownership(path)[1], ['plain'])

    def test_gltf_shared_and_baked(self):
        mesh = {'primitives': [{'positions': POINTS.tolist(), 'indices': TRIANGLES.ravel().tolist()}]}
        baked = {'primitives': [{'positions': (POINTS + [8, 2, 1]).tolist(), 'indices': TRIANGLES.ravel().tolist()}]}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'source.glb'
            path.write_bytes(build_glb([{'mesh': 0, 'name': 'first'}, {'mesh': 0, 'translation': [4, 0, 0]},
                                        {'mesh': 1}], [], meshes=[mesh, baked]))
            src = load_gltf_mesh(path)
            src.tri_o, src.object_names = gltf_ownership(path)
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
                load_object_source(settings)

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
            objects = masked_objects(src, SimpleNamespace(alpha_keep=0.5))
        self.assertEqual([obj.name for obj in objects], ['first'])

    def test_alpha_partial_copy_no_longer_matches(self):
        with tempfile.TemporaryDirectory() as directory:
            src = self.alpha_source(directory)
            src.tri_t[-1] = 0
            objects = masked_objects(src, SimpleNamespace(alpha_keep=0.5))
        self.assertEqual([len(obj.triangles) for obj in objects], [4, 3])
        self.assertEqual(len(match_instances(objects)), 2)

    def test_alpha_none_keeps_everything(self):
        with tempfile.TemporaryDirectory() as directory:
            src = self.alpha_source(directory)
            src.tri_t[:] = 0
            objects = masked_objects(src, SimpleNamespace(alpha_keep=None))
        self.assertEqual([len(obj.triangles) for obj in objects], [4, 4])
        self.assertEqual(len(match_instances(objects)), 1)

    def test_load_obj_source_alignment(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory))
            settings.source['path'].write_text(obj_text([('first', POINTS, TRIANGLES),
                                                        ('second', POINTS + 10, TRIANGLES)]))
            src = load_object_source(settings)
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
            src = load_object_source(settings)
        self.assertEqual(src.object_names, ['first', 'node_1'])
        np.testing.assert_array_equal(src.tri_o, np.repeat([0, 1], 4))
        np.testing.assert_array_equal(src.p[src.tri_v[src.tri_o == 1]], (POINTS + [10, 0, 0])[TRIANGLES])

    def test_load_source_count_mismatch_raises(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory))
            with patch('r3d.objects.source.obj_ownership', return_value=(np.array([0]), ['bad'])):
                with self.assertRaisesRegex(ValueError, 'ownership disagree'):
                    load_object_source(settings)

    def run_pose(self, objects, budget=None, matrix=None, position=None):
        job = SimpleNamespace(renderer=SimpleNamespace(variant=SimpleNamespace(triangles=budget)),
                              object=SimpleNamespace(matrix=np.eye(3) if matrix is None else matrix,
                                                     position=np.zeros(3) if position is None else position))
        with patch('r3d.objects.objects_report.camera_path_poses',
                   return_value=(184, 224, 1., 0.1, np.array([[0., 0., 0., 0., 0., -1.]]))):
            return pose_rows(match_instances(objects), None, job, 1000, (184, 224))

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


if __name__ == '__main__':
    unittest.main()
