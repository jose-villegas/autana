"""Carved products retain copy lighting and expand into placed renderers."""

import pathlib
import sys
import tempfile
import struct
from unittest import mock
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import numpy as np
from r3d.fit import stage
from r3d import build_pack, lcol_asset, mesh_bundle, mesh_import, mesh_asset, scene_asset
from build.name_hash import name_hash
from asset.asset_pack import NAME_BYTES, PackError, build_pack as encode_pack, parse_pack
from test_scene_asset import SceneFiles
from test_r3d_import import SIMPLIFY, VARIANT, renderer, write_import
from test_r3d_objects import POINTS, TRIANGLES, obj_text
from r3d.lit_mesh import read_lit_mesh
from r3d.objects.instances import Instance
from r3d.import_settings import MESHLET_TRIANGLES, albedo_jobs, load_import_settings
from tests.test_r3d_bake import canonical


def bundle_fixture(directory, flat=False):
    p = np.array([[0., 0, 0], [4, 0, 0], [0, 4, 0]])
    t = np.array([[0, 1, 2]])
    rgb = np.full(p.shape, 80)
    labels = np.zeros(len(t), dtype=int)
    rule = {'match': {'material': 'prop'}, 'triangles': 1}
    copies = []
    for index, mirrored in enumerate((False, False, True)):
        linear = np.diag([-1 if mirrored else 1, 1, 1])
        at = np.array([index * 8., 0, 0])
        instance = Instance(str(index), linear, at, mirrored, 0.)
        faces = t[:, ::-1] if mirrored else t
        copies.append((instance, (p @ linear.T + at, rgb + index, faces, labels)))
    shape = dict(positions=p, tris=t, materials=labels, rule=rule, copies=copies)
    geometry = SimpleNamespace(positions=p, rgb=rgb, tris=t, tri_double=labels, tri_mat=labels,
                               scale={}, shapes=[shape], src=SimpleNamespace(names=['prop'], object_names=['a', 'b', 'c']))
    obj = SimpleNamespace(name='atrium', matrix=np.diag([2., 3, 4]).tolist(), position=[5., 6, 7],
                          kind='mesh_renderer', animator=None)
    renderer = SimpleNamespace(face_samples=(1, 1, 1, None) if flat else None, bake=True)
    job = SimpleNamespace(object=obj, renderer=renderer, asset_name='hall.atrium',
                          settings=SimpleNamespace(meshlet_triangles=MESHLET_TRIANGLES, double_sided=[]))
    scene = SimpleNamespace(objects=[obj], renderers=[job], lights=[])
    with mock.patch.object(mesh_import, 'flat_colours', side_effect=[np.full((1, 3), value) for value in (80, 80, 81, 82)]):
        mesh_import.write_baked(job, scene, pathlib.Path(directory), job.asset_name, geometry=geometry)
    return (pathlib.Path(directory) / (job.asset_name + mesh_bundle.SUFFIX)).read_bytes(), copies, geometry, job, scene



class MeshBundleTests(unittest.TestCase):
    def test_manifest_has_plain_names_without_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            data = bundle_fixture(directory)[0]
        _, copies = mesh_bundle.decode(data)
        raw = parse_pack(data)[mesh_bundle.MANIFEST][1]
        self.assertEqual(raw[mesh_bundle.HEADER.size:mesh_bundle.HEADER.size + NAME_BYTES],
                         copies[0]['name'].encode('ascii').ljust(NAME_BYTES, b'\0'))
        stride = NAME_BYTES * 2 + struct.calcsize('<12fH') + mesh_bundle.METADATA.size
        self.assertEqual(mesh_bundle.HEADER.unpack_from(raw)[3], mesh_bundle.HEADER.size + len(copies) * stride)
        self.assertEqual(mesh_bundle.decode_manifest(mesh_bundle.encode_manifest(copies)), copies)

    def check_products(self, flat):
        with tempfile.TemporaryDirectory() as directory:
            data, copies, geometry, job, scene = bundle_fixture(directory, flat)
            entries, manifest = mesh_bundle.decode(data)
            blobs = {name: (kind, data) for name, kind, data in entries}
            self.assertEqual(len(manifest), len(copies))
            self.assertEqual(len({row['mesh'] for row in manifest}), 2)
            self.assertEqual(len({row['name'] for row in manifest}), len(copies))
            for mesh_id in {row['mesh'] for row in manifest}:
                path = pathlib.Path(directory) / 'read.mesh'
                path.write_bytes(blobs[mesh_id][1])
                mesh = read_lit_mesh(path)
                self.assertIsNone(mesh.rgb)
                self.assertIsNone(mesh.face_colors)
                encoding, count, sets = lcol_asset.decode(blobs[lcol_asset.name_for_mesh(mesh_id)][1])
                self.assertEqual(encoding, lcol_asset.RGB565 if flat else lcol_asset.RGB888)
                self.assertEqual(count, len(mesh.tris) if flat else len(mesh.pos))
                members = [row for row in manifest if row['mesh'] == mesh_id]
                self.assertEqual(len(sets), len(members))
                for row in members:
                    self.assertGreater(np.linalg.det(row['matrix']), 0)
                    world = (mesh.pos / mesh.position_scale) @ np.asarray(row['matrix']).T + row['position']
                    _, (original, _, faces, _) = copies[manifest.index(row)]
                    self.assertEqual(canonical(np.rint(world * mesh.position_scale), mesh.tris),
                                     canonical(np.rint(original * mesh.position_scale), faces))
                    expected = np.full((count, 3), 80 + manifest.index(row))
                    packed = mesh_import.panel_colour(expected).tobytes() if flat else expected.astype('u1').tobytes()
                    self.assertEqual(sets[row['colour_set']], packed)
            self.assertTrue(all(row["owner"] == job.object.name for row in manifest))
            self.assertEqual([row["source"] for row in manifest], geometry.src.object_names)

    def test_smooth_shapes_have_one_set_per_copy(self):
        self.check_products(False)

    def test_flat_shapes_have_one_set_per_copy(self):
        self.check_products(True)

    def test_repeated_bakes_are_byte_identical(self):
        with tempfile.TemporaryDirectory() as directory:
            first = bundle_fixture(directory)[0]
            second = bundle_fixture(directory)[0]
        self.assertEqual(first, second)

    def check_whole_import(self, all_carved=False):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = write_import(root, output='[output]\n', body=SIMPLIFY + VARIANT +
                                f'[[objects.meshes]]\nmatch = {{ object = "{"*" if all_carved else "prop*"}" }}\ntriangles = 4\nseam = "free"\n')
            (root / 'm.obj').write_text(obj_text([('prop_a', POINTS, TRIANGLES),
                                                 ('prop_b', POINTS + [8, 0, 0], TRIANGLES),
                                                 ('rest', POINTS * 2 + [16, 0, 0], TRIANGLES)]))
            job = albedo_jobs(load_import_settings(path))[0]
            merged = mesh_import.bake_geometry(job, None)
            mesh_import.write_baked(job, None, root, job.asset_name)
            output = root / (job.asset_name + mesh_bundle.SUFFIX)
            first = output.read_bytes()
            mesh_import.write_baked(job, None, root, job.asset_name)
            self.assertEqual(first, output.read_bytes())
            entries, copies = mesh_bundle.decode(first)
            self.assertEqual(len(copies), 3 if all_carved else 2)
            blobs = {name: data for name, kind, data in entries if kind == mesh_asset.TYPE}
            actual = canonical(np.rint(merged.positions * mesh_import.POSITION_SCALE), merged.tris)
            emitted = actual.copy()
            emitted.clear()
            rest = [dict(mesh=job.asset_name, matrix=np.eye(3), position=np.zeros(3))] if job.asset_name in blobs else []
            for row in [*rest, *copies]:
                back = root / 'read.mesh'
                back.write_bytes(blobs[row['mesh']])
                mesh = read_lit_mesh(back)
                world = mesh.pos @ np.asarray(row['matrix']).T + np.asarray(row['position']) * mesh.position_scale
                emitted.update(canonical(np.rint(world), mesh.tris))
            self.assertEqual(emitted, actual)

    def test_whole_import_preserves_merged_geometry(self):
        self.check_whole_import()

    def test_an_import_can_carve_every_source_object(self):
        self.check_whole_import(all_carved=True)

    def test_fit_changes_only_the_rest_of_a_start_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            data, _, _, job, scene = bundle_fixture(directory)
            start = work / (job.asset_name + '.start' + mesh_bundle.SUFFIX)
            start.write_bytes(data)
            job.renderer.variant = SimpleNamespace(name='fixture')
            job.renderer.fit = SimpleNamespace(steps=1, batch=1, laplacian=0, normal_weight=0,
                                               surface_weight=0, surface_margin=0, budget=1)
            target = work / (job.asset_name + mesh_bundle.SUFFIX)
            def fit_main(args):
                raw = pathlib.Path(args[args.index('--start') + 1])
                self.assertEqual(raw.suffix, '.mesh')
                self.assertEqual(args[args.index('--copies') + 1], str(start))
                self.assertEqual(args[args.index('--copy-poses') + 1], str(work / 'held_out.txt'))
                blob = bytearray(raw.read_bytes())
                blob[mesh_asset.read_mesh_header(blob).colors_at] ^= 1
                out = pathlib.Path(args[args.index('--out') + 1])
                out.mkdir()
                (out / raw.name).write_bytes(blob)
            with mock.patch.dict(sys.modules, {'r3d.appearance_simplify': SimpleNamespace(main=fit_main)}):
                stage.fit(work / 'scene.scene.toml', scene, job, work, start=start, target=target)
            before, after = parse_pack(data), parse_pack(target.read_bytes())
            self.assertNotEqual(before.pop(job.asset_name), after.pop(job.asset_name))
            self.assertEqual(before, after)


class FitCopiesTests(unittest.TestCase):
    """The fit draws a carved start's copies where the board does, and gates what the fit did to them."""

    def placed(self, flat=False):
        from r3d.fit.copies import placed_copies

        with tempfile.TemporaryDirectory() as directory:
            data, copies, _geometry, job, _scene = bundle_fixture(directory, flat=flat)
            start = pathlib.Path(directory) / ('start' + mesh_bundle.SUFFIX)
            start.write_bytes(data)
            return placed_copies(start, flat), copies

    def test_each_copy_is_placed_under_its_transform_with_its_own_colours(self):
        mesh, copies = self.placed()
        drawn = {(tuple(np.round(corner, 6)), tuple(np.round(colour * 255))) for tri in mesh.tris
                 for corner, colour in zip(mesh.points[mesh.vertex_point[tri]], mesh.rgb[tri])}
        expected = {(tuple(np.round(corner, 6)), tuple(colour)) for _instance, (p, rgb, t, _labels) in copies
                    for tri in t for corner, colour in zip(p[tri], rgb[tri])}
        self.assertEqual(drawn, expected)
        self.assertFalse(mesh.flat)

    def test_a_flat_start_draws_each_copy_triangle_in_its_face_colour(self):
        from r3d.lit_mesh import panel_colour, rgb888

        mesh, copies = self.placed(flat=True)
        self.assertTrue(mesh.flat)
        self.assertEqual((len(mesh.rgb), len(mesh.tris)), (len(copies), len(copies)))
        face = rgb888(panel_colour(np.full((1, 3), 80)))[0]
        np.testing.assert_array_equal(np.rint(mesh.rgb * 255), np.tile(face, (len(copies), 1)))

    def test_copies_must_be_shaded_as_their_rest_is(self):
        from r3d.fit.copies import placed_copies

        with tempfile.TemporaryDirectory() as directory:
            start = pathlib.Path(directory) / ('start' + mesh_bundle.SUFFIX)
            start.write_bytes(bundle_fixture(directory, flat=True)[0])
            with self.assertRaisesRegex(ValueError, "flat"):
                placed_copies(start, flat=False)

    def test_joined_copies_index_their_own_vertices_and_points_after_the_rest(self):
        from r3d.fit.copies import joined

        mesh, _copies = self.placed()
        both = joined(mesh, mesh)
        np.testing.assert_array_equal(both.points[both.vertex_point[both.tris[len(mesh.tris):]]],
                                      mesh.points[mesh.vertex_point[mesh.tris]])
        np.testing.assert_array_equal(both.rgb[both.tris[len(mesh.tris):]], mesh.rgb[mesh.tris])
        self.assertIs(joined(mesh, None), mesh)

    def test_the_gate_fails_a_fit_that_hides_more_copy_pixels_than_its_start(self):
        from r3d.fit.copies import HIDDEN_MEAN_GROWTH, HIDDEN_WORST_GROWTH, check_hidden_copies

        # A hundred held-out poses, so one pose's growth moves the mean by a hundredth of it.
        start = np.linspace(0.0, 0.002, 100)
        worst = start.copy()
        worst[-1] += 0.99 * HIDDEN_WORST_GROWTH
        for fitted in (start + 0.99 * HIDDEN_MEAN_GROWTH, start - 0.001, worst):
            check_hidden_copies(start, fitted)
        worst[-1] += 0.02 * HIDDEN_WORST_GROWTH
        for fitted in (start + 1.01 * HIDDEN_MEAN_GROWTH, worst):
            with self.subTest(fitted=fitted), self.assertRaisesRegex(ValueError, "hides the copies"):
                check_hidden_copies(start, fitted)


class BundlePackTests(SceneFiles):
    def bundle_scene(self):
        write_import(self.root, 'a.import.toml', output='[output]\n',
                     body=SIMPLIFY + '[[variants]]\nname = "a"\ntriangles = 10\n[[objects.meshes]]\nmatch = { material = "m" }\ntriangles = 1\n')
        data = bundle_fixture(self.root)[0]
        entries, copies = mesh_bundle.decode(data)
        entries = [('a' if name == 'hall.atrium' else name, kind, blob) for name, kind, blob in entries]
        path = self.root / ('a' + mesh_bundle.SUFFIX)
        path.write_bytes(mesh_bundle.encode(entries, copies))
        return self.scene(renderer('a.import.toml', extra='variant = "a"\n')), path

    def test_pack_expands_copies_with_hashed_names(self):
        source, bundle = self.bundle_scene()
        entries = parse_pack(build_pack.pack_bytes([source])['hall'])
        products, copies = mesh_bundle.decode(bundle.read_bytes())
        self.assertEqual(set(entries), {'hall', *(name for name, _, _ in products)})
        self.assertNotIn(mesh_bundle.MANIFEST, entries)
        scene = scene_asset.decode(entries['hall'][1])
        self.assertEqual(len(scene['renderers']), 1 + len(copies))
        for copy, entity, rendered in zip(copies, scene['entities'][1:], scene['renderers'][1:]):
            self.assertEqual(entity['name'], copy['name'])
            self.assertEqual(entity['name_hash'], name_hash(copy['name']))
            self.assertEqual(entity['matrix'], copy['matrix'])
            self.assertEqual(entity['position'], copy['position'])
            self.assertEqual(rendered['mesh'], copy['mesh'])
            self.assertEqual(rendered['colour_set'], copy['colour_set'])

    def test_pack_validates_copy_colour_sets_before_discarding_the_manifest(self):
        source, bundle = self.bundle_scene()
        entries, copies = mesh_bundle.decode(bundle.read_bytes())
        copies[0]['colour_set'] = scene_asset.COLOUR_SET_OWN - 1
        bundle.write_bytes(mesh_bundle.encode(entries, copies))
        with self.assertRaisesRegex(ValueError, 'LCOL'):
            build_pack.pack_bytes([source])

    def test_pack_refuses_an_unknown_manifest_version(self):
        source, bundle = self.bundle_scene()
        entries = parse_pack(bundle.read_bytes())
        kind, raw = entries[mesh_bundle.MANIFEST]
        manifest = bytearray(raw)
        fields = list(mesh_bundle.HEADER.unpack_from(manifest))
        fields[0] = mesh_bundle.VERSION + 1
        mesh_bundle.HEADER.pack_into(manifest, 0, *fields)
        entries[mesh_bundle.MANIFEST] = kind, manifest
        bundle.write_bytes(encode_pack([(name, kind, data) for name, (kind, data) in entries.items()]))
        with self.assertRaisesRegex(PackError, 'unknown.*version'):
            build_pack.pack_bytes([source])


if __name__ == '__main__':
    unittest.main()
