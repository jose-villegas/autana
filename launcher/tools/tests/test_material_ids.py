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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.texture import uv_transfer
from r3d import material_ids, mesh_bundle
from r3d.appearance_simplify import projection
from r3d.lit_mesh import bundle_entries, build_lit_mesh, read_bundle_mesh
from r3d.ray_query import RayQuery
from asset.asset_pack import build_pack
from asset.engine_frame import mirror_matrix, mirror_point


def surfaces():
    points = np.array([[-2., -1, 0], [0, -1, 0], [0, 1, 0], [-2, 1, 0],
                       [0, -1, 0], [2, -1, 0], [2, 1, 0], [0, 1, 0]])
    tris = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 6], [4, 6, 7]])
    return SimpleNamespace(p=points, tri_v=tris, tri_m=np.array([0, 0, 1, 1]),
                           names=['pattern', 'plain'], textures=[object(), None], alpha_test=None,
                           camera_hidden=np.zeros(len(tris), bool))


class MaterialIdsTests(unittest.TestCase):
    def capture(self, names=('painted', 'plain')):
        """A real two-entry bundle, each entry drawing one copy of the same triangle."""
        from test_texture_asset import fake_scene
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = pathlib.Path(temporary.name)
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
                 step='triangle', matrix=np.eye(3).tolist(), position=[2 * copy, 0, 0])
            for frame in range(2) for copy, name in enumerate(names)
        ]
        metadata = dict(width=3, height=1, entries=rows)
        (root / 'entries.json').write_text(json.dumps(metadata))
        np.array([1, 2, 0], '<u4').tofile(root / '0000.ids')
        np.array([0, 2, 1], '<u4').tofile(root / '0001.ids')
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

    def test_source_ids_trace_distinct_materials_on_both_axes(self):
        source = surfaces()
        top = source.p / [1, 2, 1] + [0, 0.5, 0]
        source.p = np.concatenate([top, top - [0, 1, 0]])
        source.tri_v = np.concatenate([source.tri_v, source.tri_v + len(top)])
        source.tri_m = np.repeat(np.arange(4), 2)
        matrix = projection([0, 0, 2], [0, 0, -1], 4, 2, 0.5, 0.1)
        actual = material_ids.source_ids(source, RayQuery(source.p, source.tri_v), [(matrix, (4, 2))])[0]
        np.testing.assert_array_equal(actual, [[0, 0, 1, 1], [2, 2, 3, 3]])
        self.assertEqual(actual.dtype, np.int32)

    def test_diff_classes_and_csv_cover_every_pixel(self):
        module = material_ids
        ref = np.array([[-1, 0, 1, 1], [0, 0, -1, -1]])
        drawn = np.array([[-1, 0, 1, -1], [0, 1, 1, -1]])
        textured = np.array([[False, True, False, False], [False, False, False, False]])
        classes = module.diff_classes(ref, drawn, textured, [True, False])
        np.testing.assert_array_equal(classes, [[module.BOTH_EMPTY, module.AGREE_TEXTURED, module.AGREE_PLAIN, module.REFERENCE_ONLY],
                                               [module.MISS, module.WRONG_MATERIAL, module.DRAWN_ONLY, module.BOTH_EMPTY]])
        rows = module.material_table([ref], [drawn], [classes], ['pattern', 'plain'])
        self.assertEqual(rows[2]['drawn_only'], 1)
        self.assertEqual(rows[0]['both_empty'], 2)
        self.assertEqual(sum(sum(row[name] for name in module.CLASS_NAMES) for row in rows), ref.size)

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
        ref = material_ids.source_ids(source, RayQuery(source.p, source.tri_v), views, placement)[0]
        # The first pixel hits the left copy, the second the right copy and the third misses.
        drawn = material_ids.drawn_ids(root, bundle, source, placement)[0][0]
        np.testing.assert_array_equal(drawn, [[0, 1, -1]])
        np.testing.assert_array_equal(ref, drawn)

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

    def test_confusions_count_only_wrong_material_pairs(self):
        module = material_ids
        ref = np.array([[0, 0, 1, -1], [1, 1, -1, 0]])
        drawn = np.array([[1, 1, 0, 1], [1, 0, -1, -1]])
        classes = module.diff_classes(ref, drawn, np.zeros_like(ref, bool), [False, False])
        rows = module.material_confusions([ref, ref], [drawn, drawn], [classes, classes])
        self.assertEqual(rows, [dict(reference_material=0, drawn_material=1, pixels=4),
                                dict(reference_material=1, drawn_material=0, pixels=4)])

    def test_material_comparison_writes_sheet_csv_and_crops(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'render'))
        import render_compare
        ref = np.array([[-1, 0, 1, 0], [0, 0, -1, -1]], dtype=np.int32)
        own = np.array([[-1, 0, 1, -1], [0, 1, 1, -1]], dtype=np.int32)
        pattern = np.array([[False, True, False, False], [False, False, False, False]])
        inputs = ([ref, ref[::-1]], [own, own[::-1]], [pattern, pattern[::-1]], ['pattern', 'plain'], [True, False])
        with self.assertRaisesRegex(ValueError, 'differ in size'):
            material_ids.diff_classes(ref, own[:, :-1], pattern, [True, False])
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            out = root / 'sheet.png'
            argv = ['render_compare.py', '--material-ids', 'scene', 'object', 'poses', 'ids',
                    '--out', str(out), '--views', '1', '--crops', '1']
            with mock.patch.object(render_compare, 'material_id_inputs', return_value=inputs), \
                    mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(render_compare, 'still_sheet', wraps=render_compare.still_sheet) as layout:
                render_compare.main()
            rows = layout.call_args.args[0]
            self.assertEqual(len(rows), 1)
            self.assertEqual([title for title, _image in rows[0]],
                             ['0: reference materials', '0: drawn materials', '0: diff classes'])
            from PIL import Image
            with Image.open(out) as image:
                self.assertEqual(image.width, 3 * max(ref.shape[1], render_compare.MATERIAL_PANEL_MIN))
            with out.with_suffix('.csv').open(newline='') as handle:
                totals = list(csv.DictReader(handle))
            from r3d.material_ids import CLASS_NAMES
            columns = {name: sum(int(row[name]) for row in totals) for name in CLASS_NAMES}
            self.assertEqual(columns, dict(both_empty=4, agree_textured=2, agree_plain=2, miss=2,
                                           wrong_material=2, reference_only=2, drawn_only=2))
            self.assertEqual(sum(columns.values()), 2 * ref.size)
            self.assertEqual(int(totals[1]['reference_only']), 2)
            self.assertEqual(int(totals[1]['miss']), 2)
            self.assertEqual(int(totals[2]['miss']), 0)
            self.assertEqual(int(totals[2]['drawn_only']), 2)
            self.assertEqual(int(totals[0]['drawn_only']), 0)
            self.assertTrue(out.with_suffix('.crops.png').is_file())
            with out.with_suffix('.confusions.csv').open(newline='') as handle:
                confused = list(csv.DictReader(handle))
            self.assertEqual(confused, [dict(reference_material='0', drawn_material='1', pixels='2')])

    def test_material_crops_include_reference_only_pixels(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'render'))
        import render_compare
        ref = np.zeros((2, 4), dtype=np.int32)
        own = np.full_like(ref, material_ids.EMPTY)
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory) / 'holes.png'
            render_compare.material_id_sheet([ref], [own], [np.zeros_like(ref, bool)], ['plain'], [False],
                                              out, crops=1)
            self.assertTrue(out.with_suffix('.crops.png').is_file())

    def test_drawn_ids_refuse_invalid_ranges_and_frames(self):
        root, bundle, source, _mesh, metadata = self.capture()
        entries = root / 'entries.json'
        try:
            drawn, _ = material_ids.drawn_ids(root, bundle, source)
        except ValueError as error:
            self.fail(f"valid contiguous ranges were rejected: {error}")
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
        with self.assertRaisesRegex(ValueError, 'without gaps'):
            material_ids.drawn_ids(root, bundle, source)

    def test_drawn_ids_decode_a_real_luvs_sibling_and_refuse_a_wrong_type(self):
        from test_texture_asset import fake_scene, SIZE
        root, bundle, source, mesh, _metadata = self.capture()
        _mesh, texture_source = fake_scene()
        luvs = uv_transfer.cluster_uvs(mesh, texture_source, [0], [SIZE])
        sibling = uv_transfer.name_for_mesh('painted')
        texture_pack = root / 'texture.pack'
        for kind in (uv_transfer.TYPE, b'LMSH'):
            texture_pack.write_bytes(build_pack([(sibling, kind, uv_transfer.pack_luvs(luvs))]))
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
