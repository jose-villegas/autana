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
from r3d import build_pack, lcol_asset, lod_asset, mesh_bundle, mesh_import, mesh_asset, scene_asset, rebake
from build.name_hash import name_hash
from asset.asset_pack import NAME_BYTES, PackError, build_pack as encode_pack, parse_pack
from test_scene_asset import SceneFiles
from test_r3d_import import SIMPLIFY, VARIANT, renderer, write_import
from test_r3d_objects import POINTS, TRIANGLES, obj_text
from r3d.lit_mesh import bundle_entries, read_bundle_mesh, read_lit_mesh
from r3d.objects.instances import Instance
from r3d.process_budget import StepRecorder
from r3d.import_settings import MESHLET_TRIANGLES, albedo_jobs, load_import_settings
from tests.test_r3d_bake import canonical


def bundle_fixture(directory, flat=False, lod=False, fit=None, positions=None, triangles=None, materials=(0, 0, 0),
                   shapes=1, recorder=None):
    p = np.asarray(positions) if positions is not None else np.array([[0., 0, 0], [4, 0, 0], [0, 4, 0]])
    t = np.asarray(triangles) if triangles is not None else np.array([[0, 1, 2]])
    rgb = np.full(p.shape, 80)
    labels = np.zeros(len(t), dtype=int)
    rule = {'match': {'material': 'prop'}, 'triangles': 1}
    copies = []
    for index, mirrored in enumerate((False, False, True)):
        linear = np.diag([-1 if mirrored else 1, 1, 1])
        at = np.array([index * 8., 0, 0])
        instance = Instance(str(index), linear, at, mirrored, 0.)
        faces = t[:, ::-1] if mirrored else t
        copies.append((instance, (p @ linear.T + at, rgb + index, faces, np.full(len(t), materials[index]))))
    shape = dict(positions=p, tris=t, materials=labels, rule=rule, copies=copies)
    geometry = SimpleNamespace(positions=p, rgb=rgb, tris=t, tri_double=labels, tri_mat=labels,
                               scale={}, shapes=[dict(shape, rule={**rule, 'triangles': 1 + k}) for k in range(shapes)],
                               src=SimpleNamespace(names=['prop', 'leaf'], object_names=['a', 'b', 'c']))
    obj = SimpleNamespace(name='atrium', matrix=np.diag([2., 3, 4]).tolist(), position=[5., 6, 7],
                          kind='mesh_renderer', animator=None)
    renderer = SimpleNamespace(face_samples=(1, 1, 1, None) if flat else None, bake=True, fit=fit)
    job = SimpleNamespace(object=obj, renderer=renderer, asset_name='hall.atrium',
                          settings=SimpleNamespace(meshlet_triangles=MESHLET_TRIANGLES, double_sided=['leaf'], lod=lod))
    scene = SimpleNamespace(objects=[obj], renderers=[job], lights=[])
    with mock.patch.object(mesh_import, 'flat_colours', side_effect=[np.full((1, 3), value) for value in (80, 80, 81, 82)]):
        mesh_import.write_baked(job, scene, pathlib.Path(directory), job.asset_name, geometry=geometry, recorder=recorder)
    return (pathlib.Path(directory) / (job.asset_name + mesh_bundle.SUFFIX)).read_bytes(), copies, geometry, job, scene



class MeshBundleTests(unittest.TestCase):
    def test_a_start_with_two_shapes_sums_meshlets_over_every_mesh_and_writes_one_file(self):
        recorder = StepRecorder(probe=lambda gpu: (100, None), interval=None)
        with tempfile.TemporaryDirectory() as directory:
            data, _, geometry, job, _ = bundle_fixture(directory, shapes=2, recorder=recorder)
            written = sorted(path.name for path in pathlib.Path(directory).iterdir())
        meshes = [name for name, kind, _ in mesh_bundle.decode(data)[0] if kind == mesh_asset.TYPE]
        # The rest mesh, then each shape's plain and mirrored copy mesh, one triangle each.
        self.assertEqual(len(meshes), 5)
        self.assertEqual(written, [job.asset_name + mesh_bundle.SUFFIX])
        rest = len(geometry.tris)
        self.assertEqual([(row['step'], row['triangles_in'], row['triangles_out']) for row in recorder.rows],
                         [('meshlets', len(meshes) * rest, len(meshes) * rest), ('write', rest, rest)])

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
                path = pathlib.Path(directory) / 'read.bundle'
                path.write_bytes(blobs[mesh_id][1])
                mesh = read_lit_mesh(blobs[mesh_id][1])
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

    def test_copies_whose_materials_differ_in_sidedness_get_their_own_mesh(self):
        # Copies joined by surface keep their own materials; a double-sided one cannot share a single-sided mesh.
        with tempfile.TemporaryDirectory() as directory:
            data = bundle_fixture(directory, materials=(0, 1, 0))[0]
        entries, manifest = mesh_bundle.decode(data)
        meshes = [row['mesh'] for row in manifest]
        self.assertEqual(len(set(meshes)), 3)
        double = {row["mesh"]: {c[-1] for c in read_bundle_mesh(entries, row["mesh"]).clusters} for row in manifest}
        self.assertEqual([double[mesh] for mesh in meshes], [{False}, {True}, {False}])

    def test_smooth_shapes_have_one_set_per_copy(self):
        self.check_products(False)

    def test_flat_shapes_have_one_set_per_copy(self):
        self.check_products(True)

    def test_a_levels_sibling_with_the_wrong_type_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            data, _, _, job, _ = bundle_fixture(folder)
            entries, copies = mesh_bundle.decode(data)
            entry = lod_asset.encode([(mesh_asset.LOD_NONE, 0)], [(0, 0, 0, 1, 0)])
            entries.append((lod_asset.name_for_mesh(job.asset_name), b"TEST", entry))
            with self.assertRaisesRegex(PackError, "LLOD"):
                mesh_bundle.decode(mesh_bundle.encode(entries, copies))

    def test_own_coloured_rest_does_not_need_room_for_a_colour_sibling(self):
        with tempfile.TemporaryDirectory() as folder:
            data, _, _, _, _ = bundle_fixture(folder)
            entries, copies = mesh_bundle.decode(data)
            name = "x" * (NAME_BYTES - len(lcol_asset.NAME_SUFFIX))
            renamed = mesh_bundle.rename_rest(entries, copies, name)
            self.assertIn(name, {key for key, _, _ in renamed})
            mesh_bundle.decode(mesh_bundle.encode(renamed, copies))

    def test_rebaking_rest_preserves_copy_geometry_levels_and_lighting(self):
        from test_r3d_lod_bake import wavy_grid
        points, _, triangles, _ = wavy_grid()
        with tempfile.TemporaryDirectory() as folder:
            data, _, _, job, _ = bundle_fixture(folder, lod=True, positions=points, triangles=triangles)
            before, copies = mesh_bundle.decode(data)
            path = pathlib.Path(folder) / (job.asset_name + mesh_bundle.SUFFIX)
            path.write_bytes(data)
            rebake.main([str(path), "--meshlet-triangles", str(MESHLET_TRIANGLES // 2)])
            after, placed = mesh_bundle.decode(path.read_bytes())
            changed = {job.asset_name, lod_asset.name_for_mesh(job.asset_name)}
            self.assertEqual([row for row in before if row[0] not in changed],
                             [row for row in after if row[0] not in changed])
            self.assertEqual(placed, copies)

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
                back = root / 'read.bundle'
                back.write_bytes(blobs[row['mesh']])
                mesh = read_lit_mesh(blobs[row["mesh"]])
                world = mesh.pos @ np.asarray(row['matrix']).T + np.asarray(row['position']) * mesh.position_scale
                emitted.update(canonical(np.rint(world), mesh.tris))
            self.assertEqual(emitted, actual)

    def test_whole_import_preserves_merged_geometry(self):
        self.check_whole_import()

    def test_an_import_can_carve_every_source_object(self):
        self.check_whole_import(all_carved=True)

    def test_a_fitted_renderer_s_start_bakes_its_rest_without_levels_and_its_copies_with_them(self):
        baked = []
        real = mesh_import.build_lit_mesh

        def recording(*args, **options):
            baked.append(options["lod"])
            return real(*args, **options)
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(mesh_import, "build_lit_mesh", recording):
            bundle_fixture(directory, lod=True, fit=SimpleNamespace())
        self.assertFalse(baked[0])
        self.assertTrue(len(baked) > 1 and all(baked[1:]))

    def test_the_fit_bakes_levels_when_the_import_asks_for_them(self):
        for lod in (False, True):
            with self.subTest(lod=lod), tempfile.TemporaryDirectory() as directory:
                work = pathlib.Path(directory)
                data, _, _, job, scene = bundle_fixture(directory)
                job.settings.lod = lod
                start = work / (job.asset_name + '.start' + mesh_bundle.SUFFIX)
                start.write_bytes(data)
                job.renderer.variant = SimpleNamespace(name='fixture')
                job.renderer.fit = SimpleNamespace(steps=1, batch=1, laplacian=0, normal_weight=0,
                                                   surface_weight=0, surface_margin=0, budget=1)
                asked = []

                def fit_main(args):
                    asked.append('--lod' in args)
                    raw = pathlib.Path(args[args.index('--start') + 1])
                    out = pathlib.Path(args[args.index('--out') + 1])
                    out.mkdir()
                    (out / raw.name).write_bytes(raw.read_bytes())
                with mock.patch.dict(sys.modules, {'r3d.appearance_simplify': SimpleNamespace(main=fit_main)}):
                    stage.fit(work / 'scene.scene.toml', scene, job, work, start=start,
                              target=work / (job.asset_name + mesh_bundle.SUFFIX))
                self.assertEqual(asked, [False])

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
                self.assertEqual(raw.suffix, '.bundle')
                self.assertEqual(args[args.index('--copies') + 1], str(start))
                self.assertEqual(args[args.index('--copy-poses') + 1], str(work / 'held_out.txt'))
                products, copied = mesh_bundle.decode(raw.read_bytes())
                blob = bytearray(products[0][2])
                blob[mesh_asset.read_mesh_header(blob).colors_at] ^= 1
                out = pathlib.Path(args[args.index('--out') + 1])
                out.mkdir()
                (out / raw.name).write_bytes(mesh_bundle.encode([(products[0][0], mesh_asset.TYPE, blob)], copied))
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

    def test_copy_levels_draw_the_finest_clusters_as_stored(self):
        from r3d.fit.copies import placed_copies
        from r3d.lod.bake_levels import bake_lit_mesh
        from test_r3d_lod_bake import wavy_grid
        mesh = bake_lit_mesh(*wavy_grid())
        self.assertIsNotNone(mesh.lod)
        shown = [c for c, (own, _) in zip(mesh.clusters, mesh.lod) if own == mesh_asset.LOD_NONE]
        vertices = np.concatenate([np.arange(vbase, vbase + vcount) for vbase, vcount, *_ in shown])
        faces = np.concatenate([mesh.tris[tbase:tbase + tcount] for _, _, tbase, tcount, *_ in shown])
        colours = lcol_asset.encode(lcol_asset.RGB888, len(mesh.pos), [mesh.rgb.astype('u1').tobytes()])
        mesh.rgb = None
        with tempfile.TemporaryDirectory() as folder:
            data, _, _, _, _ = bundle_fixture(folder)
            _, copies = mesh_bundle.decode(data)
            row = {**copies[0], "mesh": "prototype", "colour_set": 0}
            entries = bundle_entries("prototype", mesh)
            entries.append((lcol_asset.name_for_mesh("prototype"), lcol_asset.TYPE, colours))
            path = pathlib.Path(folder) / 'copy.bundle'
            path.write_bytes(mesh_bundle.encode(entries, [row]))
            placed = placed_copies(path, flat=False)
        # The finest clusters' own vertices and triangles, in the bundle's order: nothing welded by value.
        np.testing.assert_array_equal(vertices[placed.tris], faces)
        self.assertEqual(len(placed.vertex_point), len(vertices))

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
