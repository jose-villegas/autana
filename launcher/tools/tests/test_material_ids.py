"""Material attribution and mismatch counts over synthetic source surfaces."""
import importlib
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


def surfaces():
    points = np.array([[-2., -1, 0], [0, -1, 0], [0, 1, 0], [-2, 1, 0],
                       [0, -1, 0], [2, -1, 0], [2, 1, 0], [0, 1, 0]])
    tris = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 6], [4, 6, 7]])
    return SimpleNamespace(p=points, tri_v=tris, tri_m=np.array([0, 0, 1, 1]),
                           names=['pattern', 'plain'], textures=[object(), None], alpha_test=None,
                           camera_hidden=np.zeros(len(tris), bool))


class MaterialIdsTests(unittest.TestCase):
    def module(self):
        try:
            return importlib.import_module('r3d.material_ids')
        except ImportError:
            self.fail('material_ids is missing')

    def test_cluster_uvs_preserves_fixture_bytes(self):
        from test_texture_asset import fake_scene, SIZE
        expected = {
            (0, 0): 'ce3fcb6da0c2d64dc3396080ee4a39a997120bbd7e1f267916a675d517374437',
            (0, 1): '839e60ee9f8e61937157a59e97181a8e33991aca56b4c93e2d0dab21ae78b4c7',
            (1, 1): '6502dd32fd8fca75b812094c5f7b2426fd4274ad9e81a87e44409f9beafc23c4',
        }
        for materials, digest in expected.items():
            mesh, source = fake_scene(materials=materials)
            data = uv_transfer.pack_luvs(uv_transfer.cluster_uvs(mesh, source, [0], [SIZE]))
            self.assertEqual(hashlib.sha256(data).hexdigest(), digest)

    def test_nearest_materials_keeps_backfacing_material(self):
        source = surfaces()
        self.assertTrue(hasattr(uv_transfer, 'nearest_materials'))
        near, material, facing = uv_transfer.nearest_materials(source.p, source.tri_v[:, ::-1], source)
        np.testing.assert_array_equal(material, source.tri_m)
        self.assertFalse(facing.any())
        np.testing.assert_array_equal(near, np.arange(len(source.tri_v)))

    def test_source_ids_trace_the_two_materials(self):
        module = self.module()
        from r3d.ray_query import RayQuery
        source = surfaces()
        with tempfile.TemporaryDirectory() as directory:
            poses = pathlib.Path(directory) / 'poses.txt'
            poses.write_text('size 4 2\nlens 0.5 0.1\npose 0 0 2 0 0 -1\n')
            actual = module.source_ids(source, RayQuery(source.p, source.tri_v), poses)[0]
        np.testing.assert_array_equal(actual, [[0, 0, 1, 1], [0, 0, 1, 1]])
        self.assertEqual(actual.dtype, np.int32)

    def test_diff_classes_and_csv_cover_every_pixel(self):
        module = self.module()
        ref = np.array([[-1, 0, 1], [0, 0, -1]])
        drawn = np.array([[-1, 0, 1], [0, 1, 1]])
        textured = np.array([[False, True, False], [False, False, False]])
        classes = module.diff_classes(ref, drawn, textured, [True, False])
        np.testing.assert_array_equal(classes, [[module.BOTH_EMPTY, module.AGREE_TEXTURED, module.AGREE_PLAIN],
                                               [module.MISS, module.WRONG_MATERIAL, module.ONE_EMPTY]])
        rows = module.material_table([ref], [classes], ['pattern', 'plain'])
        self.assertEqual(sum(sum(row[name] for name in module.CLASS_NAMES) for row in rows), ref.size)

    def test_placed_entry_returns_to_source_frame(self):
        module = self.module()
        from asset.engine_frame import AXIS_SIGNS
        source = surfaces()
        placement = SimpleNamespace(matrix=np.diag([2., 3., 4.]), position=np.array([5., 6., 7.]))
        signs = np.asarray(AXIS_SIGNS)
        row = dict(matrix=(placement.matrix * signs[:, None] * signs[None, :]).tolist(),
                   position=(placement.position * signs).tolist())
        mesh = SimpleNamespace(pos=source.p, position_scale=1)
        np.testing.assert_allclose(module.entry_points(mesh, row, placement), source.p)
        source.material_placement = placement
        with tempfile.TemporaryDirectory() as directory:
            poses = pathlib.Path(directory) / 'poses.txt'
            poses.write_text('size 4 2\nlens 0.5 0.1\npose 5 6 11 0 0 -1\n')
            from r3d.ray_query import RayQuery
            ref = module.source_ids(source, RayQuery(source.p, source.tri_v), poses)[0]
        np.testing.assert_array_equal(ref, [[0, 0, 1, 1], [0, 0, 1, 1]])

    def test_copy_entry_points_match_placed_copies(self):
        module = self.module()
        from asset.engine_frame import AXIS_SIGNS
        from r3d import mesh_bundle
        from r3d.fit.copies import placed_copies
        from r3d.lit_mesh import read_bundle_mesh
        from test_mesh_bundle import bundle_fixture
        signs = np.asarray(AXIS_SIGNS)
        with tempfile.TemporaryDirectory() as directory:
            data = bundle_fixture(directory)[0]
            path = pathlib.Path(directory) / 'copy.bundle'
            path.write_bytes(data)
            entries, copies = mesh_bundle.decode(data)
            expected = placed_copies(path, False).points
            actual = []
            for row in copies:
                engine = dict(matrix=(np.asarray(row['matrix']) * signs[:, None] * signs[None, :]).tolist(),
                              position=(np.asarray(row['position']) * signs).tolist())
                actual.append(module.entry_points(read_bundle_mesh(entries, row['mesh']), engine))
        np.testing.assert_allclose(np.unique(np.concatenate(actual), axis=0), np.unique(expected, axis=0))

    def test_material_comparison_writes_sheet_csv_and_crops(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'render'))
        import render_compare
        self.assertTrue(hasattr(render_compare, 'material_id_inputs'))
        ref = np.array([[0, 0, 1, 1], [0, 0, 1, 1]], dtype=np.int32)
        drawn = [ref.copy(), np.zeros_like(ref)]
        inputs = ([ref, ref], drawn, [np.zeros_like(ref, bool)] * 2, ['pattern', 'plain'], [True, False])
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            out = root / 'sheet.png'
            argv = ['render_compare.py', '--material-ids', 'scene', 'object', 'poses', 'ids',
                    '--out', str(out), '--views', '1', '--crops', '1']
            with mock.patch.object(render_compare, 'material_id_inputs', return_value=inputs), \
                    mock.patch.object(sys, 'argv', argv):
                render_compare.main()
            from PIL import Image
            with Image.open(out) as image:
                self.assertEqual(image.width, 3 * max(ref.shape[1], render_compare.MATERIAL_PANEL_MIN))
            self.assertTrue(out.with_suffix('.csv').is_file())
            self.assertTrue(out.with_suffix('.crops.png').is_file())

    def test_drawn_ids_use_triangle_codes_and_entry_ranges(self):
        module = self.module()
        source = surfaces()
        mesh = SimpleNamespace(pos=source.p, tris=source.tri_v, position_scale=1)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'entries.json').write_text(json.dumps(dict(width=3, height=1, entries=[
                dict(frame=0, entry='mesh', first_id=1, triangle_count=4, id_count=4, step='triangle',
                     matrix=np.eye(3).tolist(), position=[0, 0, 0])], texture_pack='texture.pack')))
            np.array([1, 3, 0], dtype='<u4').tofile(root / '0000.ids')
            codes = np.array([uv_transfer.PLAIN, 1, 1, uv_transfer.PLAIN])
            with mock.patch('r3d.material_ids.mesh_bundle.decode', return_value=([('mesh', b'LMSH', b'')], [])), \
                    mock.patch('r3d.material_ids.read_bundle_mesh', return_value=mesh), \
                    mock.patch('r3d.material_ids.parse_pack', return_value={
                        uv_transfer.name_for_mesh('mesh'): (uv_transfer.TYPE, b'uvs')}), \
                    mock.patch('r3d.material_ids.uv_transfer.decode', return_value=SimpleNamespace(codes=codes)):
                (root / 'bundle').write_bytes(b'')
                (root / 'texture.pack').write_bytes(b'')
                drawn, textured = module.drawn_ids(root, root / 'bundle', source)
            np.testing.assert_array_equal(drawn[0], [[0, 1, -1]])
            np.testing.assert_array_equal(textured[0], [[False, True, False]])


if __name__ == '__main__':
    unittest.main()
