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
from scipy.spatial import cKDTree
from scipy.spatial.transform import Rotation

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from gltf.gltf_write import build_glb
from r3d.gltf_mesh import load_gltf_mesh
from r3d.obj import load_obj
from r3d.import_settings import SettingsError, load_import_settings
from r3d.objects.instances import MATCH_TOLERANCE, match_instances, source_objects
from r3d.objects.objects_report import LOD_LEVELS, LOD_TRIANGLE_RATIO, main, pose_rows
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


def triangle_set(triangles, materials):
    return sorted((tuple(sorted(map(int, face))), int(material)) for face, material in zip(triangles, materials))


class ObjectsTests(unittest.TestCase):
    def test_obj_rigid_and_reordered_instances(self):
        rotation = Rotation.from_rotvec(np.array([1., 2., 3.]) * 0.37).as_matrix()
        permutation = np.array([2, 0, 3, 1])
        distorted = POINTS.copy()
        distorted[1, 0] += 0.1
        copies = [('original', POINTS, TRIANGLES),
                  ('rotated', POINTS @ rotation.T + [7, 4, 9], TRIANGLES),
                  ('mirrored', POINTS * [-1, 1, 1] + [12, 0, 0], TRIANGLES),
                  ('shuffled', (POINTS @ rotation.T + [1, 8, 2])[permutation],
                   np.argsort(permutation)[TRIANGLES[::-1]]),
                  ('unrelated', POINTS * [2, 1, 1], TRIANGLES),
                  ('distorted', distorted, TRIANGLES), ('scaled', POINTS * 2, TRIANGLES)]
        # Reordering faces must carry their materials too.
        text = obj_text(copies)
        start = text.index('o shuffled')
        end = text.index('o unrelated')
        shuffled = text[start:end].replace('material_0', 'material_tmp').replace('material_1', 'material_0').replace('material_tmp', 'material_1')
        text = text[:start] + shuffled + text[end:]
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'source.obj'
            path.write_text(text)
            p, _, tv, _, tm, _ = load_obj(path)
            to, names = obj_ownership(path)
            self.assertEqual(len(to), len(tv))
        objects = source_objects(p, tv, tm, to, names)
        shapes = match_instances(objects)
        self.assertEqual(len(shapes), 4)
        self.assertEqual([len(shape.instances) for shape in shapes], [4, 1, 1, 1])
        by_name = {obj.name: obj for obj in objects}
        for shape in shapes:
            np.testing.assert_array_equal(shape.instances[0].linear, np.eye(3))
            for instance in shape.instances:
                obj = by_name[instance.object_name]
                placed = shape.positions @ instance.linear.T + instance.position
                distances, mapping = cKDTree(obj.positions).query(placed)
                self.assertLessEqual(max(distances), MATCH_TOLERANCE * np.linalg.norm(np.ptp(obj.positions, axis=0)))
                self.assertEqual(triangle_set(mapping[shape.triangles], shape.materials), triangle_set(obj.triangles, obj.materials))
                self.assertEqual(instance.mirrored, instance.object_name == 'mirrored')
        again = match_instances(objects)
        for a, b in zip(shapes, again):
            for ia, ib in zip(a.instances, b.instances):
                np.testing.assert_array_equal(ia.linear, ib.linear)
                np.testing.assert_array_equal(ia.position, ib.position)

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

    def test_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_import(root)
            (root / 'm.obj').write_text(obj_text([('inside', POINTS + [0, 0, -10], TRIANGLES),
                                                 ('outside', POINTS + [10000, 0, -10], TRIANGLES)]))
            scene = write_scene(root, renderer() + camera(path=True, region=False))
            out = root / 'report'
            main([str(scene), '--object', 'mesh', '--out', str(out), '--every-ms', '1000'])
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
            self.assertGreaterEqual(float(rows[0]['triangles_with_lod']), 4 / LOD_TRIANGLE_RATIO ** (LOD_LEVELS - 1))
            self.assertLessEqual(float(rows[0]['triangles_with_lod']), 4)
            self.assertEqual(float(rows[0]['budget_triangles']), 8)
            self.assertEqual(float(rows[0]['saved']), 4 - float(rows[0]['triangles_with_lod']))
            summary = (out / 'objects.md').read_text()
            self.assertIn('Calibrated pixels per triangle:', summary)
            self.assertIn('| Triangles LOD frees |', summary)
            self.assertNotIn('LOD_FULL_RADIUS_PX', summary)


if __name__ == '__main__':
    unittest.main()
