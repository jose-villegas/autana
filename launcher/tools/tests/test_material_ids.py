"""Material attribution and mismatch counts over synthetic source surfaces."""
import csv
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.texture import uv_transfer
from r3d import material_ids, mesh_bundle
from r3d.appearance_simplify import projection
from r3d.lit_mesh import bundle_entries, build_lit_mesh, read_bundle_mesh
from r3d.ray_query import RayQuery
from r3d.reference_render import primary_hits
from tests.r3d_env import needs_llvm

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'render'))
import render_compare

POSE_HEADER = 'frame,ms,px,py,pz,qx,qy,qz,qw\n'


def write_pose_log(root, count, eye_z=0):
    (root / 'capture.poses.csv').write_text(
        POSE_HEADER + ''.join(f'{i},0,0,0,{-eye_z},0,0,0,1\n' for i in range(count)))
from asset.asset_pack import build_pack
from asset.engine_frame import mirror_matrix, mirror_point


def surfaces():
    points = np.array([[-2., -1, 0], [0, -1, 0], [0, 1, 0], [-2, 1, 0],
                       [0, -1, 0], [2, -1, 0], [2, 1, 0], [0, 1, 0]])
    tris = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 6], [4, 6, 7]])
    return SimpleNamespace(p=points, tri_v=tris, tri_m=np.array([0, 0, 1, 1]),
                           names=['pattern', 'plain'], textures=[object(), None], alpha_test=None,
                           tri_o=np.array([0, 1, 2, 2]), object_names=['wall', 'trim1', 'frame'],
                           camera_hidden=np.zeros(len(tris), bool))


class MaterialIdsTests(unittest.TestCase):
    def temporary_root(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        return pathlib.Path(temporary.name)

    def capture(self, names=('painted', 'plain')):
        """A real two-entry bundle, each entry drawing one copy of the same triangle."""
        from test_texture_asset import fake_scene
        root = self.temporary_root()
        mesh, source = fake_scene(materials=(0, 1))
        mesh = build_lit_mesh(source.p, mesh.rgb, mesh.tris[:1], np.array([True]))
        entries = [entry for name in dict.fromkeys(names) for entry in bundle_entries(name, mesh)]
        bundle = root / 'capture.bundle'
        bundle.write_bytes(mesh_bundle.encode(entries, []))
        # The other copy lies over a separate source triangle of material 1.
        source.p = np.concatenate([source.p, source.p + [2, 0, 0]])
        source.tri_v = np.stack([source.tri_v[0], source.tri_v[0] + len(source.p) // 2])
        rows = [
            dict(frame=frame, entry=name, first_id=copy + 1, triangle_count=1, id_count=1,
                 step='triangle', matrix=np.eye(3).tolist(), position=[2 * copy, 0, 0],
                 bundle_sha256=hashlib.sha256(bundle.read_bytes()).hexdigest(), texture_sha256=None)
            for frame in range(2) for copy, name in enumerate(names)
        ]
        metadata = dict(width=3, height=1, entries=rows)
        (root / 'entries.json').write_text(json.dumps(metadata))
        np.array([1, 2, 0], '<u4').tofile(root / '0000.ids')
        np.array([0, 2, 1], '<u4').tofile(root / '0001.ids')
        write_pose_log(root, 2)
        return root, bundle, source, mesh, metadata

    def test_cluster_uvs_preserves_fixture_bytes(self):
        from test_texture_asset import fake_scene, SIZE
        # Digests measured with origin/main's cluster_uvs.
        expected = {
            (0, 0): 'ce3fcb6da0c2d64dc3396080ee4a39a997120bbd7e1f267916a675d517374437',
            (0, 1): '839e60ee9f8e61937157a59e97181a8e33991aca56b4c93e2d0dab21ae78b4c7',
            (1, 1): '6502dd32fd8fca75b812094c5f7b2426fd4274ad9e81a87e44409f9beafc23c4',
        }
        for materials, digest in expected.items():
            mesh, source = fake_scene(materials=materials)
            data = uv_transfer.pack_luvs(uv_transfer.cluster_uvs(mesh, source, [0], [SIZE]))
            self.assertEqual(hashlib.sha256(data).hexdigest(), digest)

        mesh, source = fake_scene(materials=(0, 1))
        mesh.tris = np.concatenate([mesh.tris, mesh.tris[:1, ::-1]])
        mesh.clusters = [(0, 4, 0, len(mesh.tris), *mesh.clusters[0][4:])]
        _near, _material, facing, *_geometry = uv_transfer.nearest_materials(
            mesh.pos / mesh.position_scale, mesh.tris, source)
        np.testing.assert_array_equal(facing, [True, True, False])
        charts = uv_transfer.cluster_uvs(mesh, source, [0], [SIZE])
        np.testing.assert_array_equal(charts.codes[1:], [uv_transfer.PLAIN, uv_transfer.PLAIN])
        data = uv_transfer.pack_luvs(charts)
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         'e543d2cc578771c2c65dd0e1733bdc7940496c2aa4365d58bd2d19edfaca1269')

    def test_nearest_materials_keeps_backfacing_material(self):
        source = surfaces()
        near, material, facing, *_geometry = uv_transfer.nearest_materials(source.p, source.tri_v[:, ::-1], source)
        np.testing.assert_array_equal(material, source.tri_m)
        self.assertFalse(facing.any())
        np.testing.assert_array_equal(near, np.arange(len(source.tri_v)))

    @needs_llvm
    def test_source_ids_trace_distinct_materials_on_both_axes(self):
        source = surfaces()
        top = source.p / [1, 2, 1] + [0, 0.5, 0]
        source.p = np.concatenate([top, top - [0, 1, 0]])
        source.tri_v = np.concatenate([source.tri_v, source.tri_v + len(top)])
        source.tri_m = np.repeat(np.arange(4), 2)
        source.tri_o = np.tile([0, 0, 1, 1], 2)
        matrix = projection([0, 0, 2], [0, 0, -1], 4, 2, 0.5, 0.1)
        materials, objects = material_ids.source_ids(source, RayQuery(source.p, source.tri_v), [(matrix, (4, 2))])
        np.testing.assert_array_equal(materials[0], [[0, 0, 1, 1], [2, 2, 3, 3]])
        np.testing.assert_array_equal(objects[0], [[0, 0, 1, 1], [0, 0, 1, 1]])
        self.assertEqual(materials[0].dtype, np.int32)

    def test_diff_classes_and_csv_cover_every_pixel(self):
        module = material_ids
        ref = np.array([[0, 0, 1, 0, 1, 0, -1, -1]])
        drawn = np.array([[0, 0, 1, -1, 0, 0, 1, -1]])
        textured = np.array([[True, False, False, False, False, False, False, False]])
        empty = module.exclusion_ids(surfaces(), [ref], [ref], [])[0]
        classes = module.diff_classes(ref, drawn, textured, [True, False], empty)
        np.testing.assert_array_equal(classes, [[module.AGREE_TEXTURED, module.MISS, module.AGREE_PLAIN,
                                                module.REFERENCE_ONLY, module.WRONG_MATERIAL, module.MISS,
                                                module.DRAWN_ONLY, module.BOTH_EMPTY]])
        rows = module.material_table([ref], [drawn], [classes], ['pattern', 'plain'])
        self.assertEqual(rows[2]['drawn_only'], 1)
        self.assertEqual(rows[0]['both_empty'], 1)
        self.assertEqual([sum(row[name] for row in rows) for name in module.CLASS_NAMES], [1, 1, 1, 2, 1, 1, 1, 0])

    @needs_llvm
    def test_placed_entry_and_reference_agree_through_an_asymmetric_rotation(self):
        root, bundle, source, mesh, metadata = self.capture()
        rotation = np.array([[0., 0., 1.], [0., 1., 0.], [-1., 0., 0.]])
        placement = SimpleNamespace(matrix=rotation @ np.diag([2., 3., 4.]), position=np.array([5., 6., 7.]))
        for row in metadata['entries']:
            position = placement.matrix @ row['position'] + placement.position
            row.update(matrix=mirror_matrix(placement.matrix).tolist(), position=list(mirror_point(position)))
        (root / 'entries.json').write_text(json.dumps(metadata))
        points = mesh.pos / mesh.position_scale
        np.testing.assert_allclose(material_ids.entry_points(mesh, metadata['entries'][0], placement), points)

        world = np.eye(4)
        world[:3, :3], world[:3, 3] = placement.matrix, placement.position
        size = (3, 1)
        local_clip = projection([2.5, 0.2, 1], [0, 0, -1], *size, 1.0, 0.1)
        views = [(local_clip @ np.linalg.inv(world), size)]
        source.tri_o = np.arange(len(source.tri_v))
        ref, _objects = material_ids.source_ids(source, RayQuery(source.p, source.tri_v), views, placement)
        # The first pixel hits the left copy, the second the right copy and the third misses.
        drawn = material_ids.drawn_ids(root, bundle, source, placement)[0][0]
        np.testing.assert_array_equal(drawn, [[0, 1, -1]])
        np.testing.assert_array_equal(ref[0], drawn)

    def test_copy_entry_points_match_each_placed_copy_in_order(self):
        root, bundle, source, _mesh, metadata = self.capture(('copy', 'copy'))
        rows = [row for row in metadata['entries'] if row['frame'] == 0]
        self.assertEqual(len(rows), 2)
        entries, _copies = mesh_bundle.decode(bundle.read_bytes())
        mesh = read_bundle_mesh(entries, 'copy')
        original = mesh.pos / mesh.position_scale
        for copy, row in enumerate(rows):
            with self.subTest(copy=copy):
                np.testing.assert_allclose(material_ids.entry_points(mesh, row), original + [2 * copy, 0, 0])
        drawn, _textured = material_ids.drawn_ids(root, bundle, source)
        np.testing.assert_array_equal(drawn[0], [[0, 1, -1]])
        np.testing.assert_array_equal(drawn[1], [[-1, 1, 0]])

    def test_exclusion_selectors_match_reference_pixels(self):
        source = surfaces()
        source.p[3] = source.p[0] + [0, 0.1, 0]
        source.textures = [SimpleNamespace(levels=[np.zeros((1, 1, 4))]), None]
        materials = [np.array([[0, 1, 0, 1, -1]])]
        objects = [np.array([[0, 0, 1, 2, -1]])]
        cases = (
            (('--exclude-material', 'patt*'), [0, -1, 0, -1, -1]),
            (('--exclude-object', 'trim?'), [-1, -1, 0, -1, -1]),
            (('--exclude', 'alpha-tested'), [0, -1, 0, -1, -1]),
            (('--exclude', 'thin=0.25'), [-1, -1, 0, -1, -1]),
            (('--exclude', 'not-drawn'), [-1, -1, -1, 0, -1]),
        )
        for selector, expected in cases:
            with self.subTest(selector=selector):
                actual = material_ids.exclusion_ids(source, materials, objects, [selector], {0, 1})
                np.testing.assert_array_equal(actual[0], [expected])

    def test_not_drawn_excludes_the_dropped_objects_wrong_material_pixels(self):
        root, bundle, source, _mesh, metadata = self.capture()
        source.tri_o = np.array([0, 1])
        source.object_names = ['kept', 'dropped']
        entries, _copies = mesh_bundle.decode(bundle.read_bytes())
        bundle.write_bytes(mesh_bundle.encode([entry for entry in entries if entry[0] == 'painted'], []))
        metadata['entries'] = [dict(row, bundle_sha256=hashlib.sha256(bundle.read_bytes()).hexdigest())
                               for row in metadata['entries'] if row['entry'] == 'painted']
        (root / 'entries.json').write_text(json.dumps(metadata))
        for frame in range(2):
            np.array([1, 1, 0], '<u4').tofile(root / f'{frame:04d}.ids')
        owners = set()
        drawn, pattern = material_ids.drawn_ids(root, bundle, source, drawn_objects=owners)
        ref = np.array([[0, 1, -1]])
        before = material_ids.diff_classes(ref, drawn[0], pattern[0], [False, False])
        self.assertEqual(np.count_nonzero(before == material_ids.WRONG_MATERIAL), 1)
        exclusions = material_ids.exclusion_ids(source, [ref], [ref], [('--exclude', 'not-drawn')], owners)
        after = material_ids.diff_classes(ref, drawn[0], pattern[0], [False, False], exclusions[0])
        np.testing.assert_array_equal(after, [[material_ids.AGREE_PLAIN, material_ids.EXCLUDED,
                                              material_ids.BOTH_EMPTY]])
        self.assertEqual(material_ids.material_confusions([ref], drawn[:1], [after]), [])

    def test_drawn_card_uses_embedded_luvs_and_bundle_coverage_counts_uncaptured_meshes(self):
        from r3d import lcol_asset
        from r3d.texture import texture_asset
        from test_texture_asset import fake_scene, SIZE

        root, bundle, source, mesh, metadata = self.capture(('rest', 'rest'))
        source.p = np.concatenate([source.p, source.p + [4, 0, 0]])
        source.tri_v = np.concatenate([source.tri_v, source.tri_v + len(source.p) // 2])
        source.tri_m = np.array([0, 1, 0, 1])
        source.tri_o = np.arange(4)
        source.object_names = ['rest', 'copy', 'card', 'dropped']
        entries, _copies = mesh_bundle.decode(bundle.read_bytes())
        copies = []
        for name, x in [('copy', 2), ('card', 4)]:
            copy_mesh = build_lit_mesh(source.p, np.full(source.p.shape, 128), source.tri_v[:1],
                                       np.array([True]), colourless=True)
            entries.extend(bundle_entries(name, copy_mesh))
            entries.append((lcol_asset.name_for_mesh(name), lcol_asset.TYPE,
                            lcol_asset.encode(lcol_asset.RGB888, len(copy_mesh.pos), [bytes(copy_mesh.pos.size)])))
            copies.append(dict(name=name + '.place', mesh=name, colour_set=0, owner='fixture',
                               source='dropped', first='fixture', match={}, matrix=np.eye(3).tolist(),
                               position=[x, 0, 0]))
        _, texture_source = fake_scene()
        luvs = uv_transfer.cluster_uvs(mesh, texture_source, [0], [SIZE])
        texture = texture_asset.mask_texture(np.ones((SIZE, SIZE), dtype=bool), 1)
        entries.extend([(uv_transfer.name_for_mesh('card'), uv_transfer.TYPE, uv_transfer.pack_luvs(luvs)),
                        (uv_transfer.texture_name('card', 0), texture_asset.TYPE, texture_asset.pack_ltex(texture))])
        bundle.write_bytes(mesh_bundle.encode(entries, copies))
        metadata['entries'] = [dict(row, entry='card', position=[4, 0, 0],
                                    bundle_sha256=hashlib.sha256(bundle.read_bytes()).hexdigest())
                               for row in metadata['entries'] if row['first_id'] == 1]
        (root / 'entries.json').write_text(json.dumps(metadata))
        for frame in range(2):
            np.array([1, 0, 0], dtype='<u4').tofile(root / f'{frame:04d}.ids')
        owners = set()
        with mock.patch.object(uv_transfer, 'material_search', wraps=uv_transfer.material_search) as search, \
                mock.patch.object(uv_transfer, 'nearest_materials', wraps=uv_transfer.nearest_materials) as mapping:
            drawn, textured = material_ids.drawn_ids(root, bundle, source, drawn_objects=owners)
        self.assertEqual((search.call_count, mapping.call_count), (1, 3))
        self.assertEqual(owners, {0, 1, 2})
        self.assertEqual(luvs.codes[0], 1)
        diff = material_ids.diff_classes(drawn[0], drawn[0], textured[0], [True, False])
        self.assertEqual(diff[0, 0], material_ids.AGREE_TEXTURED)

    def test_excluding_material_preserves_classes_on_other_pixels(self):
        module = material_ids
        source = surfaces()
        ref = np.array([[0, 1, 1, 1, 1, 1, -1, -1]])
        drawn = np.array([[1, 1, 1, 0, -1, 1, 0, -1]])
        pattern = np.array([[False, False, True, False, False, False, False, False]])
        before = module.diff_classes(ref, drawn, pattern, [False, True])
        excluded = module.exclusion_ids(source, [ref], [ref], [('--exclude-material', 'pattern')])[0]
        after = module.diff_classes(ref, drawn, pattern, [False, True], excluded)
        np.testing.assert_array_equal(after[:, 1:], before[:, 1:])
        self.assertEqual(after[0, 0], module.EXCLUDED)

    @needs_llvm
    def test_sheet_and_csvs_record_ordered_selectors_and_first_match(self):
        scene, poses, root = self.comparison_files()
        out = root / 'selectors.png'
        selectors = ['--exclude-object', '*', '--exclude-material', 'front',
                     '--exclude', 'thin=5e-1', '--exclude-object', 'missing']
        argv = ['render_compare.py', '--material-ids', str(scene), 'painted', str(poses), str(root),
                '--out', str(out), *selectors]
        import shlex
        header = 'Exclusions: ' + shlex.join(selectors)
        with (mock.patch.object(sys, 'argv', argv),
              mock.patch.object(render_compare, 'captioned', wraps=render_compare.captioned) as caption):
            render_compare.main()
        self.assertIn(header, [call.args[1] for call in caption.call_args_list])
        reports = {}
        for suffix in ('.csv', '.confusions.csv', '.excluded.csv'):
            with out.with_suffix(suffix).open(newline='') as handle:
                self.assertEqual(handle.readline().rstrip(), '# ' + header)
                reports[suffix] = list(csv.DictReader(handle))
        self.assertEqual(reports['.confusions.csv'], [])
        front = next(row for row in reports['.csv'] if row['name'] == 'front')
        self.assertEqual((front['excluded'], front['agree_plain']), ('1', '0'))
        self.assertEqual(reports['.excluded.csv'], [
            dict(selector="--exclude-object '*'", object='0', name='painted', material='front', pixels='1')])

    def test_confusions_count_only_wrong_material_pairs(self):
        module = material_ids
        ref = np.array([[0, 0, 1, -1], [1, 1, -1, 0]])
        drawn = np.array([[1, 1, 0, 1], [1, 0, -1, -1]])
        classes = module.diff_classes(ref, drawn, np.zeros_like(ref, bool), [False, False])
        rows = module.material_confusions([ref, ref], [drawn, drawn], [classes, classes])
        self.assertEqual(rows, [dict(reference_material=0, drawn_material=1, pixels=4),
                                dict(reference_material=1, drawn_material=0, pixels=4)])

    def comparison_files(self, parented=False):
        root = self.temporary_root()
        scene = root / 'room.scene.toml'
        (root / 'painted.import.toml').write_text(
            '[source]\npath = "painted.obj"\ncredit = "fixture"\n[output]\nname = "painted"\n')
        (root / 'painted.obj').write_text(
            'v -2 -2 0.5\nv 2 -2 0.5\nv 0 2 0.5\n'
            'v -2 -2 0\nv 2 -2 0\nv 0 2 0\n'
            'usemtl back\nf 1 3 2\nusemtl front\nf 4 5 6\n')
        (root / 'painted.mtl').write_text('newmtl back\nKd 1 0 0\nnewmtl front\nKd 0 1 0\n')
        from r3d.mesh_source import load_source
        from r3d.import_settings import load_import_settings
        source = load_source(load_import_settings(root / 'painted.import.toml'))
        mesh = build_lit_mesh(source.p, np.full(source.p.shape, 128, dtype=np.uint8),
                              source.tri_v[1:], np.array([False]))
        bundle = root / 'painted.bundle'
        bundle.write_bytes(mesh_bundle.encode(bundle_entries('painted', mesh), []))
        objects = ''
        placement = np.eye(3)
        position = np.zeros(3)
        if parented:
            objects = ('[[objects]]\nname = "parent"\nposition = [4.5, 0, -4]\nrotation = [0, 0, 90]\n'
                       'scale = [2, 3, 4]\n[objects.camera]\nhalf_fov_short_tan = 0.5\nnear_z = 0.1\n'
                       'background = 0\n')
            placement = np.array([[0, -9, 0], [4, 0, 0], [0, 0, 16]])
            position = np.array([0, 0, -4])
        scene.write_text(objects + '[[objects]]\nname = "painted"\n' +
                         ('parent = "parent"\nposition = [0, 1.5, 0]\nscale = [2, 3, 4]\n' if parented else '') +
                         '[objects.mesh_renderer]\nmesh = "painted.import.toml"\n')
        poses = root / 'poses.txt'
        poses.write_text('size 1 1\nlens 0.5 0.1\npose 0 0 2 0 0 -1\n')
        write_pose_log(root, 1, eye_z=2)
        np.array([1], '<u4').tofile(root / '0000.ids')
        (root / 'entries.json').write_text(json.dumps(dict(
            width=1, height=1, viewport_width=1, viewport_height=1, lens=0.5, near=0.1, entries=[
                dict(frame=0, entry='painted', first_id=1, id_count=1, triangle_count=1, step='triangle',
                     matrix=mirror_matrix(placement).tolist(), position=list(mirror_point(position.astype(float))),
                     bundle_sha256=hashlib.sha256(bundle.read_bytes()).hexdigest(), texture_sha256=None)])))
        return scene, poses, root

    @needs_llvm
    def test_material_comparison_loads_real_inputs_and_culls_the_back_face(self):
        scene, poses, root = self.comparison_files()
        out = root / 'sheet.png'
        argv = ['render_compare.py', '--material-ids', str(scene), 'painted', str(poses), str(root),
                '--out', str(out), '--views', '1']
        from r3d.import_settings import load_scene
        from r3d.reference_render import source_for
        source, job = source_for(load_scene(scene), 'painted')
        hits = primary_hits(source, job, [0, 0, 2, 0, 0, -1], 1, 1, 0.5, 1)[4]
        np.testing.assert_array_equal(source.tri_m[hits], [1])
        with mock.patch.object(sys, 'argv', argv):
            render_compare.main()
        self.assertTrue(out.is_file())
        with out.with_suffix('.csv').open(newline='') as handle:
            self.assertEqual(handle.readline().rstrip(), '# Exclusions: (none)')
            totals = {row['name']: row for row in csv.DictReader(handle)}
        self.assertEqual(int(totals['front']['agree_plain']), 1)
        with out.with_suffix('.confusions.csv').open(newline='') as handle:
            self.assertEqual(handle.readline().rstrip(), '# Exclusions: (none)')
            self.assertEqual(list(csv.DictReader(handle)), [])

    @needs_llvm
    def test_material_comparison_uses_the_parent_placement(self):
        scene, poses, root = self.comparison_files(parented=True)
        # Move the camera behind the local back face but in front of the world back face.
        poses.write_text('size 1 1\nlens 0.5 0.1\npose 0 0 -2 0 0 -1\n')
        write_pose_log(root, 1, eye_z=-2)
        ref, drawn, _pattern, names, _has_texture, _excluded = render_compare.material_id_inputs(scene, 'painted', poses, root)
        np.testing.assert_array_equal(ref[0], [[names.index('front')]])
        np.testing.assert_array_equal(ref[0], drawn[0])

    def test_material_comparison_rejects_reference_lens_mismatch(self):
        scene, poses, root = self.comparison_files()
        poses.write_text(poses.read_text().replace('lens 0.5', 'lens 1.0'))
        with (mock.patch('r3d.ray_query.RayQuery'), mock.patch.object(material_ids, 'source_ids', return_value=([], [])),
              self.assertRaisesRegex(ValueError, r'lens.*0\.5.*1\.0')):
            render_compare.material_id_inputs(scene, 'painted', poses, root)

    def test_invalid_selector_fails_before_loading_material_inputs(self):
        argv = ['render_compare.py', '--material-ids', 'scene', 'object', 'poses', 'ids',
                '--out', 'sheet.png', '--exclude', 'thin=abc']
        with (mock.patch.object(sys, 'argv', argv), mock.patch.object(render_compare, 'material_id_inputs') as load,
              mock.patch('sys.stderr') as error, self.assertRaises(SystemExit) as raised):
            render_compare.main()
        load.assert_not_called()
        self.assertEqual(raised.exception.code, 2)
        self.assertIn('argument --exclude:', error.write.call_args.args[0])

    def test_drawn_ids_refuse_same_count_bundle_and_texture_mismatches(self):
        root, bundle, source, mesh, _metadata = self.capture()
        different = root / 'different.bundle'
        mesh.pos = mesh.pos.copy()
        mesh.pos[:, 0] += 1
        different.write_bytes(mesh_bundle.encode(
            [entry for name in ('painted', 'plain') for entry in bundle_entries(name, mesh)], []))
        with self.assertRaisesRegex(ValueError, 'bundle SHA-256'):
            material_ids.drawn_ids(root, different, source)
        texture = root / 'other.pack'
        texture.write_bytes(build_pack([]))
        with self.assertRaisesRegex(ValueError, 'texture pack SHA-256'):
            material_ids.drawn_ids(root, bundle, source, texture_pack=texture)

    def test_drawn_ids_sort_numeric_frames(self):
        root, bundle, source, _mesh, _metadata = self.capture()
        count = 10001
        write_pose_log(root, count)
        frame = np.zeros(3, dtype='<u4').tobytes()
        for i in range(2, count):
            (root / f'{i:04d}.ids').write_bytes(frame)
        drawn, _pattern = material_ids.drawn_ids(root, bundle, source)
        self.assertEqual(len(drawn), count)

    def test_drawn_ids_refuse_files_left_by_a_longer_capture(self):
        root, bundle, source, _mesh, metadata = self.capture()
        write_pose_log(root, 1)
        metadata['entries'] = [row for row in metadata['entries'] if row['frame'] == 0]
        (root / 'entries.json').write_text(json.dumps(metadata))
        np.zeros(3, dtype='<u4').tofile(root / '0001.ids')
        with self.assertRaisesRegex(ValueError, 'pose log'):
            material_ids.drawn_ids(root, bundle, source)

    def test_drawn_ids_reuse_mesh_placements_and_source_search(self):
        root, bundle, source, _mesh, _metadata = self.capture(('copy', 'copy'))
        with mock.patch.object(uv_transfer, 'triangle_search', wraps=uv_transfer.triangle_search) as search, \
                mock.patch.object(uv_transfer, 'nearest_materials', wraps=uv_transfer.nearest_materials) as mapping, \
                mock.patch('r3d.lit_mesh.read_bundle_mesh', wraps=read_bundle_mesh) as read_mesh:
            material_ids.drawn_ids(root, bundle, source)
        self.assertEqual((search.call_count, mapping.call_count, read_mesh.call_count), (1, 2, 1))

    def test_material_crops_include_reference_only_pixels_and_disappear_when_excluded(self):
        ref = np.zeros((2, 4), dtype=np.int32)
        own = np.full_like(ref, material_ids.EMPTY)
        out = self.temporary_root() / 'holes.png'
        args = ([ref], [own], [np.zeros_like(ref, bool)], ['plain'], [False], out)
        render_compare.material_id_sheet(*args, crops=1)
        self.assertTrue(out.with_suffix('.crops.png').is_file())
        selectors = [(material_ids.EXCLUDE_MATERIAL, 'plain')]
        excluded = material_ids.exclusion_ids(SimpleNamespace(names=['plain']), [ref], [ref], selectors)
        render_compare.material_id_sheet(*args, crops=1, selectors=selectors, exclusions=(excluded, ()))
        self.assertFalse(out.with_suffix('.crops.png').exists())

    def test_material_views_rank_reference_only_ahead_of_single_wrong_material(self):
        ref = np.zeros((1, 2), dtype=np.int32)
        root = self.temporary_root()
        Image.new('RGB', (2, 1)).save(root / '0001.png')
        out = root / 'sheet.png'
        inputs = ([ref, ref], [np.array([[0, 1]]), np.full_like(ref, material_ids.EMPTY)],
                  [np.zeros_like(ref, bool)] * 2, ['plain', 'other'], [False, False], (None, ()))
        argv = ['render_compare.py', '--material-ids', 'scene', 'object', 'poses', 'ids',
                '--out', str(out), '--reference', str(root), '--views', '1']
        with (mock.patch.object(sys, 'argv', argv),
              mock.patch.object(render_compare, 'material_id_inputs', return_value=inputs)):
            render_compare.main()
            self.assertTrue(out.is_file())
            before = list(root.iterdir())
            argv[-1] = '2'
            with self.assertRaisesRegex(ValueError, 'missing reference image for frame 0: .*0000.png'):
                render_compare.main()
            self.assertEqual(list(root.iterdir()), before)

    def test_drawn_ids_refuse_invalid_ranges_and_frames(self):
        root, bundle, source, _mesh, metadata = self.capture()
        entries = root / 'entries.json'
        drawn, _ = material_ids.drawn_ids(root, bundle, source)
        self.assertEqual(len(drawn), 2)
        np.zeros(3, dtype='<u4').tofile(root / '0000.ids')
        for change, reason in ((dict(first_id=1), 'overlap'), (dict(first_id=3), 'gap'),
                               (dict(id_count=2), 'count differs'), (dict(triangle_count=2), 'count differs'),
                               (dict(step='cluster'), 'requires --view triangles')):
            with self.subTest(change=change):
                rows = metadata['entries'].copy()
                rows[1] = dict(rows[1], **change)
                entries.write_text(json.dumps(dict(metadata, entries=rows)))
                with self.assertRaisesRegex(ValueError, reason):
                    material_ids.drawn_ids(root, bundle, source)
        entries.write_text(json.dumps(metadata))
        np.array([1, 3, 0], '<u4').tofile(root / '0000.ids')
        with self.assertRaisesRegex(ValueError, 'outside captured entry ranges'):
            material_ids.drawn_ids(root, bundle, source)
        np.array([1, 2, 0], '<u4').tofile(root / '0000.ids')
        (root / '0001.ids').rename(root / '0002.ids')
        with self.assertRaisesRegex(ValueError, 'pose log'):
            material_ids.drawn_ids(root, bundle, source)

    def test_drawn_ids_decode_a_real_luvs_sibling_and_refuse_a_wrong_type(self):
        from test_texture_asset import fake_scene, SIZE
        root, bundle, source, mesh, metadata = self.capture()
        _mesh, texture_source = fake_scene()
        luvs = uv_transfer.cluster_uvs(mesh, texture_source, [0], [SIZE])
        sibling = uv_transfer.name_for_mesh('painted')
        texture_pack = root / 'texture.pack'
        for kind in (uv_transfer.TYPE, b'LMSH'):
            texture_pack.write_bytes(build_pack([(sibling, kind, uv_transfer.pack_luvs(luvs))]))
            for row in metadata['entries']:
                row['texture_sha256'] = hashlib.sha256(texture_pack.read_bytes()).hexdigest()
            (root / 'entries.json').write_text(json.dumps(metadata))
            if kind == uv_transfer.TYPE:
                drawn, textured = material_ids.drawn_ids(root, bundle, source, texture_pack=texture_pack)
                np.testing.assert_array_equal(drawn[0], [[0, 1, -1]])
                np.testing.assert_array_equal(textured[0], [[True, False, False]])
                np.testing.assert_array_equal(textured[1], [[False, False, True]])
            else:
                with self.assertRaisesRegex(ValueError, 'not LUVS'):
                    material_ids.drawn_ids(root, bundle, source, texture_pack=texture_pack)


if __name__ == '__main__':
    unittest.main()
