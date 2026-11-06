"""Synthetic source objects, rigid placements and offline scene reporting."""

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
from r3d.objects.instances import (MATCH_TOLERANCE, SourceObject, bounds_diagonal, match_instances, place, rigid_fit,
                                   source_objects)
from r3d.objects.surface_match import FLOOR_CAP, SURFACE_TOLERANCE, limit, nearest, surface
from r3d.objects.object_error import (PLANAR_AREA_SHARE, PLANAR_DEGREES, SHEET_DEGREES, SHEET_GROWTH, SKY, Parts,
                                      cell, common_prefix, object_rows, part_labels, part_names, planar_parts, read,
                                      nearest_source, report_body, rows_for, shape_rows, sheet_triangles, write)
from r3d.objects.objects_report import main, masked_objects
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

    def test_zero_diagonal_report(self):
        from r3d.objects.objects_report import write_report

        obj = SourceObject('point', np.zeros_like(POINTS), TRIANGLES, MATERIALS)
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory)
            write_report(out, [obj, obj], match_instances([obj, obj]), MATCH_TOLERANCE)
            self.assertTrue((out / 'residuals.png').is_file())

    def test_cli_tolerance_reaches_matching(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_import(root)
            scene = write_scene(root, renderer() + camera(path=True, region=False))
            with patch('r3d.objects.objects_report.match_instances', wraps=match_instances) as matching:
                main([str(scene), '--object', 'mesh', '--out', str(root / 'out'), '--tolerance', '0.0123'])
            self.assertEqual(matching.call_args.args[1], 0.0123)

    def test_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_import(root)
            (root / 'm.obj').write_text(obj_text([('inside', POINTS + [0, 0, -10], TRIANGLES),
                                                 ('outside', POINTS + [10000, 0, -10], TRIANGLES)]))
            scene = write_scene(root, renderer() + camera(path=True, region=False))
            out = root / 'report'
            main([str(scene), '--object', 'mesh', '--out', str(out)])
            for name in ('objects.json', 'objects.md', 'residuals.png'):
                self.assertTrue((out / name).is_file(), name)
            data = json.loads((out / 'objects.json').read_text())
            self.assertEqual(data['totals']['objects'], 2)
            self.assertEqual(data['totals']['shapes'], 1)
            self.assertEqual(data['totals']['source_triangles'], 8)
            self.assertEqual(data['totals']['unique_triangles'], 4)
            self.assertEqual(data['totals']['repeated_shapes'], 1)


def subdivided(points, triangles):
    """Every triangle split in four at its edge midpoints: the same surface, triangulated differently."""
    points, faces, middle = [np.asarray(p, dtype=np.float64) for p in points], [], {}

    def midpoint(a, b):
        key = (min(a, b), max(a, b))
        if key not in middle:
            middle[key] = len(points)
            points.append((points[a] + points[b]) / 2)
        return middle[key]

    for a, b, c in triangles:
        ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
        faces += [[a, ab, ca], [ab, b, bc], [ca, bc, c], [ab, bc, ca]]
    return np.array(points), np.array(faces)


FINE_POINTS, FINE_TRIANGLES = subdivided(POINTS, TRIANGLES)
TURN = Rotation.from_rotvec([0.4, 0.8, 1.2]).as_matrix()
MIRROR = np.diag([-1., 1., 1.])


def surface_shapes(*copies, **options):
    """The original tetrahedron and each (name, points, triangles[, materials]) copy, matched by surface."""
    objects = [SourceObject('original', POINTS, TRIANGLES, MATERIALS)]
    objects += [SourceObject(name, points, triangles, rest[0] if rest else np.zeros(len(triangles), dtype=int))
                for name, points, triangles, *rest in copies]
    return match_instances(objects, surface_tolerance=options.pop('tolerance', SURFACE_TOLERANCE), **options)


def lifted(height):
    """The tetrahedron with its apex raised by height: a surface off the original by about that much."""
    return POINTS + np.array([[0., 0., 0.]] * 3 + [[0., 0., height]])


class SurfaceMatchTests(unittest.TestCase):
    def test_retriangulated_copy_joins_only_by_surface(self):
        copy = ('fine', FINE_POINTS @ TURN.T + [5, 1, 2], FINE_TRIANGLES)
        self.assertEqual(len(match_instances([SourceObject('original', POINTS, TRIANGLES, MATERIALS),
                                              SourceObject(*copy, np.zeros(len(FINE_TRIANGLES), dtype=int))])), 2)
        [shape] = surface_shapes(copy)
        joined = next(i for i in shape.instances if i.triangle_order == ())
        self.assertFalse(joined.mirrored)
        self.assertLess(joined.gap, 1e-6)

    def test_joined_copy_places_the_shape_on_its_surface(self):
        for name, points, triangles in (('turned', FINE_POINTS @ TURN.T + [5, 1, 2], FINE_TRIANGLES),
                                        ('mirrored', FINE_POINTS @ MIRROR @ TURN.T, FINE_TRIANGLES[:, ::-1])):
            with self.subTest(name):
                [shape] = surface_shapes((name, points, triangles))
                for instance in shape.instances:
                    target = surface(points, triangles) if instance.object_name == name else surface(POINTS, TRIANGLES)
                    placed = place(surface(shape.positions, shape.triangles).samples, instance.linear,
                                   instance.position)
                    self.assertLess(nearest(placed, target)[0].max(), 1e-5)
                self.assertEqual(sorted(i.mirrored for i in shape.instances), [False, name == 'mirrored'])

    def test_materials_are_shading_not_shape(self):
        [shape] = surface_shapes(('painted', FINE_POINTS + [9, 0, 0], FINE_TRIANGLES,
                                  np.arange(len(FINE_TRIANGLES)) % 3 + 7))
        self.assertEqual(len(shape.instances), 2)

    def test_tolerance_boundary(self):
        allowed = SURFACE_TOLERANCE * bounds_diagonal(POINTS)
        self.assertEqual(len(surface_shapes(('near', lifted(0.5 * allowed), TRIANGLES))), 1)
        self.assertEqual(len(surface_shapes(('far', lifted(3 * allowed), TRIANGLES))), 2)

    def test_floor_loosens_up_to_its_cap(self):
        diagonal = bounds_diagonal(POINTS)
        beyond_tolerance = ('lifted', lifted(3 * SURFACE_TOLERANCE * diagonal), TRIANGLES)
        self.assertEqual(len(surface_shapes(beyond_tolerance)), 2)
        self.assertEqual(len(surface_shapes(beyond_tolerance, surface_floor=FLOOR_CAP * diagonal)), 1)
        # A lift past the cap also moves the area out of the compared bucket, so the cap is read off limit().
        tetrahedron = surface(POINTS, TRIANGLES)
        self.assertAlmostEqual(limit(tetrahedron, tetrahedron, SURFACE_TOLERANCE, 10 * diagonal),
                               FLOOR_CAP * diagonal)

    def test_same_input_same_placements(self):
        copies = [('turned', FINE_POINTS @ TURN.T + [5, 1, 2], FINE_TRIANGLES),
                  ('mirrored', FINE_POINTS @ MIRROR @ TURN.T, FINE_TRIANGLES[:, ::-1])]
        first, second = surface_shapes(*copies), surface_shapes(*copies)
        for a, b in zip(first, second):
            np.testing.assert_array_equal(a.positions, b.positions)
            for x, y in zip(a.instances, b.instances):
                self.assertEqual(x.object_name, y.object_name)
                np.testing.assert_array_equal(x.linear, y.linear)
                np.testing.assert_array_equal(x.position, y.position)

    def test_invalid_surface_options(self):
        for options in (dict(tolerance=-1.), dict(tolerance=float('nan')), dict(surface_floor=-1.),
                        dict(surface_floor=float('inf'))):
            with self.subTest(options=options), self.assertRaises(ValueError):
                surface_shapes(**options)

    def test_report_parts_and_surfaces(self):
        def obj_lines(name, points, triangles, offset):
            return ([f'o {name}', 'usemtl material_0'] + ['v ' + ' '.join(map(str, p)) for p in points]
                    + ['f ' + ' '.join(str(int(v) + offset + 1) for v in face) for face in triangles])

        # One object holding two apart copies (two parts), and a retriangulated copy of its own.
        twice = np.vstack([POINTS, POINTS + [20, 0, 0]]), np.vstack([TRIANGLES, TRIANGLES + len(POINTS)])
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_import(root)
            (root / 'm.obj').write_text('\n'.join(
                obj_lines('pair', *twice, 0) + obj_lines('fine', FINE_POINTS + [0, 9, 0], FINE_TRIANGLES, 8)) + '\n')
            scene = write_scene(root, renderer() + camera(path=True, region=False))
            out = root / 'report'
            with self.assertRaises(SystemExit):
                main([str(scene), '--object', 'mesh', '--out', str(out), '--surface-floor', '1'])
            main([str(scene), '--object', 'mesh', '--out', str(out), '--parts', '--surface-tolerance'])
            self.assertTrue((out / 'shapes.png').is_file())
            totals = json.loads((out / 'objects.json').read_text())['totals']
            self.assertEqual((totals['objects'], totals['shapes'], totals['surface_copies']), (3, 1, 1))
            self.assertEqual(totals['repeated_triangles'], 8 + len(FINE_TRIANGLES))


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


class ObjectErrorTests(unittest.TestCase):
    """Parts, repeats and the per-part split of a held-out frame's error."""

    def scene(self):
        # Object a: one tetrahedron, and its copy moved apart (two parts of one repeated shape). Object b: a larger
        # tetrahedron (its own shape) welded to a's first part at a corner: one part sharing a position with a.
        moved = POINTS + [10., 0., 0.]
        welded = 2 * POINTS + POINTS[1]
        p = np.concatenate([POINTS, moved, welded])
        tri_v = np.concatenate([TRIANGLES, TRIANGLES + 4, TRIANGLES + 8])
        tri_o = np.repeat([0, 0, 1], len(TRIANGLES))
        return p, tri_v, tri_o, np.tile(MATERIALS, 3)

    def test_parts_split_by_welded_position_within_an_object(self):
        p, tri_v, tri_o, _ = self.scene()
        labels, shared = part_labels(p, tri_v, tri_o)
        np.testing.assert_array_equal(labels, np.repeat([0, 1, 2], len(TRIANGLES)))
        np.testing.assert_array_equal(shared, [True, False, True])
        names, _, parts_of_object = part_names(labels, tri_o, ['a', 'b'])
        self.assertEqual(names, ['a#0', 'a#1', 'b'])
        np.testing.assert_array_equal(parts_of_object, [2, 2, 1])

    def test_rows_split_the_frame_error_and_rank_it(self):
        p, tri_v, tri_o, tri_m = self.scene()
        labels, shared = part_labels(p, tri_v, tri_o)
        names, owner, parts_of_object = part_names(labels, tri_o, ['a', 'b'])
        shapes = match_instances(source_objects(p, tri_v, tri_m, labels, names))
        parts = Parts(names, [['a', 'b'][o] for o in owner], parts_of_object, shared, ['m0', 'm0', 'm1'],
                      np.bincount(labels), np.array([4, 2, 2]), np.array([True, False, False]), np.array([1, 0, 2]))
        # Six pixels: two show a#0, three b, one the sky; no pixel shows a#1, so its drawn triangles are unseen.
        ids = np.array([0, 0, 2, 2, 2, -1])
        error = np.array([1., 1., 6., 6., 6., 4.])
        # A perfect match has no error to share: every share is 0, not NaN.
        perfect, perfect_score, _ = rows_for(parts, shapes, {0: (ids, np.zeros(len(ids)))})
        self.assertEqual(perfect_score, 0.0)
        self.assertEqual([row['error_share'] for row in perfect], [0.0] * len(perfect))
        rows, score, unseen = rows_for(parts, shapes, {0: (ids, error)})
        self.assertAlmostEqual(score, error.mean())
        self.assertAlmostEqual(unseen, 2 / 8)
        self.assertEqual([row['object'] for row in rows], ['b', SKY, 'a#0'])
        by_name = {row['object']: row for row in rows}
        self.assertAlmostEqual(sum(row['error_share'] for row in rows), 1.0)
        self.assertAlmostEqual(sum(row['pixel_share'] for row in rows), 1.0)
        self.assertAlmostEqual(by_name['b']['error_share'], 18 / 24)
        self.assertAlmostEqual(by_name['a#0']['mean_delta_e'], 1.0)
        self.assertEqual(by_name['a#0']['copies'], 2)
        self.assertAlmostEqual(by_name['a#0']['drawn_share'], 0.5)
        self.assertEqual(by_name['b']['material'], 'm1')
        # b is one part welded to another object, so only sub-mesh segmentation can split it; a#0 is welded but
        # already split by connectivity.
        self.assertTrue(by_name['b']['segment'])
        self.assertFalse(by_name['a#0']['segment'])
        self.assertTrue(by_name['a#0']['welded'])
        # Planar parts and sheets count over the parts in view only: a#1's are unseen.
        self.assertTrue(by_name['a#0']['planar'])
        self.assertIn(' 4 drawn triangles sit on planar parts in view; 3 are sheets', report_body(rows, score, unseen))
        [shape] = shape_rows(rows)
        self.assertEqual((shape['name'], shape['seen'], shape['placed']), ('a#0', 1, 2))
        self.assertAlmostEqual(shape['error_share'], 2 / 24)
        objects = object_rows(rows)
        self.assertEqual([(group['name'], group['seen'], group['placed']) for group in objects],
                         [('b', 1, 1), ('a', 1, 2)])
        # The page's tables are remade from the kept rows: the same tables, every field its own type again.
        with tempfile.TemporaryDirectory() as folder:
            out = pathlib.Path(folder)
            write(out, rows, score, unseen, 'title', dict(fit='f', reference='r'))
            kept, totals = read(out / 'error')
        for row, again in zip(rows, kept, strict=True):
            for key, value in row.items():
                self.assertIsInstance(again[key], float if isinstance(value, float) else type(value))
                if isinstance(value, float):
                    self.assertAlmostEqual(again[key], value, delta=1e-5 * abs(value))
                else:
                    self.assertEqual(again[key], value)
        self.assertEqual((totals['title'], totals['fit'], totals['reference']), ('title', 'f', 'r'))
        self.assertEqual(report_body(kept, totals['mean_delta_e'], totals['drawn_unseen_share']),
                         report_body(rows, score, unseen))

    @staticmethod
    def hinge(degrees):
        """Two unit right triangles sharing the edge along x, the second turned `degrees` up about it."""
        turn = np.radians(degrees)
        p = np.array([[0., 0., 0.], [1., 0., 0.], [0., 0., 1.], [0., np.sin(turn), -np.cos(turn)]])
        return p, np.array([[0, 2, 1], [0, 1, 3]])

    def test_a_part_is_planar_while_its_normals_stay_within_the_angle(self):
        # Each half of a hinge stands half its angle off the part's mean normal.
        for degrees, planar in ((2 * PLANAR_DEGREES * 0.9, True), (2 * PLANAR_DEGREES * 1.1, False)):
            p, tri_v = self.hinge(degrees)
            np.testing.assert_array_equal(planar_parts(p, tri_v, np.zeros(2, dtype=np.int64), 1), [planar])
        # A kerb's edge: a floor triangle with a small one turned straight up stays planar while the small one's
        # share of the area is under 1 - PLANAR_AREA_SHARE.
        for share, planar in (((1 - PLANAR_AREA_SHARE) * 0.9, True), ((1 - PLANAR_AREA_SHARE) * 1.1, False)):
            side = np.sqrt(share / (1 - share))
            p = np.array([[0., 0., 0.], [1., 0., 0.], [0., 0., 1.], [0., 0., 0.], [side, 0., 0.], [0., side, 0.]])
            tri_v = np.array([[0, 2, 1], [3, 4, 5]])
            np.testing.assert_array_equal(planar_parts(p, tri_v, np.zeros(2, dtype=np.int64), 1), [planar])

    def test_a_sheet_is_a_large_triangle_turned_off_the_source(self):
        # The source is one floor triangle, one part; most drawn triangles lie on it at unit size, setting the
        # part's median, and each case stands over it turned about x by its angle and scaled by its area.
        source = np.array([[0., 0., 0.], [1., 0., 0.], [0., 0., 1.]])

        def drawn(degrees, area):
            turn, scale = np.radians(degrees), np.sqrt(area)
            return scale * np.array([[0., 0., 0.], [1., 0., 0.], [0., np.sin(turn), np.cos(turn)]])

        cases = [((SHEET_DEGREES * 1.1, SHEET_GROWTH * 1.1), True), ((SHEET_DEGREES * 0.9, SHEET_GROWTH * 1.1), False),
                 ((SHEET_DEGREES * 1.1, SHEET_GROWTH * 0.9), False), ((180 - SHEET_DEGREES * 0.9, SHEET_GROWTH * 1.1), False)]
        floor = [drawn(0, 1)] * (2 * len(cases) + 1)
        triangles = np.stack(floor + [drawn(*case) for case, _ in cases])
        drawn_p, drawn_tris = triangles.reshape(-1, 3), np.arange(3 * len(triangles)).reshape(-1, 3)
        near = np.zeros(len(triangles), dtype=np.int64)
        sheets = sheet_triangles(source, np.array([[0, 1, 2]]), drawn_p, drawn_tris, near, np.zeros(1, dtype=np.int64))
        np.testing.assert_array_equal(sheets, [False] * len(floor) + [sheet for _, sheet in cases])

    def test_planar_weighs_area_not_triangle_count(self):
        # Many tiny floor triangles and one large wall: planar by count, not by area; and the reverse.
        def part(floor_side, floor_count, wall_side):
            p, tris = [], []
            for i in range(floor_count):
                p += [[i * 2.0, 0., 0.], [i * 2.0 + floor_side, 0., 0.], [i * 2.0, 0., floor_side]]
                tris.append([3 * i, 3 * i + 2, 3 * i + 1])
            p += [[0., 0., 0.], [wall_side, 0., 0.], [0., wall_side, 0.]]
            tris.append([len(p) - 3, len(p) - 2, len(p) - 1])
            return np.array(p), np.array(tris), np.zeros(len(tris), dtype=np.int64)

        # 200 floor triangles of area 0.005 against one wall of area 50.
        np.testing.assert_array_equal(planar_parts(*part(0.1, 200, 10.0), 1), [False])
        # The reverse: one large floor beside one tiny wall, half the triangles but under 1% of the area.
        np.testing.assert_array_equal(planar_parts(*part(10.0, 1, 0.1), 1), [True])

    def test_a_folded_part_is_not_planar(self):
        # Back-to-back halves cancel in the signed mean normal: neither faces it.
        p = np.array([[0., 0., 0.], [1., 0., 0.], [0., 0., 1.]])
        np.testing.assert_array_equal(planar_parts(p, np.array([[0, 2, 1], [0, 1, 2]]), np.zeros(2, dtype=np.int64),
                                                   1), [False])

    def test_sheets_compare_against_their_own_parts_median(self):
        # Two floor parts far apart: part 0 drawn with unit triangles, part 1 with triangles 10 times larger. A tilted
        # triangle of area 5 is a sheet over part 0 and not over part 1.
        source = np.array([[0., 0., 0.], [1., 0., 0.], [0., 0., 1.], [100., 0., 0.], [101., 0., 0.], [100., 0., 1.]])
        tri_v, labels = np.array([[0, 2, 1], [3, 5, 4]]), np.array([0, 1])

        def triangle(origin, area, degrees):
            turn, scale = np.radians(degrees), np.sqrt(2 * area)
            return np.array(origin) + scale * np.array([[0., 0., 0.], [1., 0., 0.], [0., np.sin(turn), np.cos(turn)]])

        floors = [triangle([0., 0., 0.], 0.5, 0)] * 5 + [triangle([100., 0., 0.], 5.0, 0)] * 5
        tilted = [triangle([0., 0., 0.], 2.5, 90), triangle([100., 0., 0.], 2.5, 90)]
        triangles = np.stack(floors + tilted)
        drawn = triangles.reshape(-1, 3), np.arange(3 * len(triangles)).reshape(-1, 3)
        near = np.array([0] * 5 + [1] * 5 + [0, 1])
        np.testing.assert_array_equal(sheet_triangles(source, tri_v, *drawn, near, labels),
                                      [False] * 10 + [True, False])

    def test_a_drawn_triangle_goes_to_the_source_nearest_its_centre(self):
        # The drawn triangle's first corner sits on source triangle 0, its centre on source triangle 1.
        unit = np.array([[0., 0., 0.], [1., 0., 0.], [0., 0., 1.]])
        source = np.concatenate([unit, unit + [10., 0., 0.]])
        drawn = np.array([[0., 0., 0.], [16., 0., 0.], [16., 0., 1.]])
        np.testing.assert_array_equal(nearest_source(source, np.array([[0, 1, 2], [3, 4, 5]]), drawn,
                                                     np.array([[0, 1, 2]])), [1])

    def test_no_drawn_triangles_count_no_sheets(self):
        source = np.array([[0., 0., 0.], [1., 0., 0.], [0., 0., 1.]])
        sheets = sheet_triangles(source, np.array([[0, 1, 2]]), np.zeros((0, 3)), np.zeros((0, 3), dtype=np.int64),
                                 np.zeros(0, dtype=np.int64), np.zeros(1, dtype=np.int64))
        self.assertEqual(sheets.shape, (0,))

    def test_report_names_leave_out_the_shared_prefix(self):
        self.assertEqual(common_prefix([SKY, 'Scene_Street_1#0', 'Scene_Stone_2']), 'Scene_')
        self.assertEqual(cell('Scene_Street_1#0', 'Scene_'), 'Street_1#0')


if __name__ == '__main__':
    unittest.main()
