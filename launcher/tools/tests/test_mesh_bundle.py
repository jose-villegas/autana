"""Carved products retain copy lighting and pack into one batch a shape."""

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
from r3d.batch import copy_batches
from build.name_hash import name_hash
from asset.asset_pack import NAME_BYTES, PackError, build_pack as encode_pack, parse_pack
from test_scene_asset import SceneFiles
from test_r3d_import import SIMPLIFY, VARIANT, renderer, write_import
from test_r3d_objects import POINTS, TRIANGLES, obj_text
from r3d.lit_mesh import read_lit_mesh
from r3d.objects.instances import Instance
from r3d.process_budget import StepRecorder
from r3d.import_settings import MESHLET_TRIANGLES, albedo_jobs, load_import_settings
from tests.test_r3d_bake import canonical


def bundle_fixture(directory, flat=False, lod=False, fit=None, positions=None, triangles=None, materials=(0, 0, 0),
                   shapes=1, recorder=None, name=None):
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
                          settings=SimpleNamespace(meshlet_triangles=MESHLET_TRIANGLES, double_sided=['leaf'], lod=lod,
                                                    fit=SimpleNamespace(probes=None, budget=None)))
    scene = SimpleNamespace(objects=[obj], renderers=[job], lights=[])
    with mock.patch.object(mesh_import, 'flat_colours', side_effect=[np.full((1, 3), value) for value in (80, 80, 81, 82)]):
        mesh_import.write_baked(job, scene, pathlib.Path(directory), name or job.asset_name, geometry=geometry,
                                recorder=recorder)
    return (pathlib.Path(directory) / ((name or job.asset_name) + mesh_bundle.SUFFIX)).read_bytes(), copies, geometry, job, scene



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
        double = {row["mesh"]: {c[-1] for c in mesh_bundle.read_mesh(entries, row["mesh"]).clusters} for row in manifest}
        self.assertEqual([double[mesh] for mesh in meshes], [{False}, {True}, {False}])

    def test_smooth_shapes_have_one_set_per_copy(self):
        self.check_products(False)

    def test_flat_shapes_have_one_set_per_copy(self):
        self.check_products(True)

    def test_a_levels_sibling_with_the_wrong_type_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            data, _, _, job, _ = bundle_fixture(folder)
            entries, copies = mesh_bundle.decode(data)
            entry = lod_asset.encode([(mesh_asset.LOD_NONE, 0)], [(0, 0, 0, 1, 0, 0)])
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

    def test_a_start_written_under_a_long_variant_name_names_its_shapes_from_the_asset(self):
        with tempfile.TemporaryDirectory() as directory:
            baked = mesh_bundle.decode(bundle_fixture(directory)[0])[1]
            named = mesh_bundle.decode(bundle_fixture(directory, name="a_fitted_variant_name")[0])[1]
        self.assertEqual([row['mesh'] for row in named], [row['mesh'] for row in baked])

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
                asked = []
                run_fit(directory, lambda args: asked.append('--lod' in args) or keep_start(args), lod=lod)
                self.assertEqual(asked, [False])

    def test_fit_changes_only_the_rest_of_a_start_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            fixture_job = bundle_fixture(directory)[3]

            def fit_main(args):
                raw = pathlib.Path(args[args.index('--start') + 1])
                self.assertEqual(raw.suffix, '.bundle')
                self.assertEqual(args[args.index('--copies') + 1], str(start_path(work, fixture_job)))
                self.assertEqual(args[args.index('--copy-poses') + 1], str(work / 'held_out.txt'))
                products, copied = mesh_bundle.decode(raw.read_bytes())
                blob = bytearray(products[0][2])
                blob[mesh_asset.read_mesh_header(blob).colors_at] ^= 1
                out = pathlib.Path(args[args.index('--out') + 1])
                out.mkdir()
                (out / raw.name).write_bytes(mesh_bundle.encode([(products[0][0], mesh_asset.TYPE, blob)], copied))
            data, job, target = run_fit(directory, fit_main)
            before, after = parse_pack(data), parse_pack(target.read_bytes())
            self.assertNotEqual(before.pop(job.asset_name), after.pop(job.asset_name))
            self.assertEqual(before, after)


def start_path(work, job):
    return work / (job.asset_name + '.start' + mesh_bundle.SUFFIX)


def keep_start(args):
    """A fit that writes its start back unchanged."""
    raw = pathlib.Path(args[args.index('--start') + 1])
    out = pathlib.Path(args[args.index('--out') + 1])
    out.mkdir()
    (out / raw.name).write_bytes(raw.read_bytes())


def run_fit(directory, fit_main, lod=None, path=True, shaping=None):
    """Runs the fit stage on the fixture's start with `fit_main` standing in for the fit, the scene camera with a
    path or without one and the import's fit `shaping` when given; returns the start's bytes, the job and the
    fitted bundle."""
    work = pathlib.Path(directory)
    data, _, _, job, scene = bundle_fixture(directory)
    scene.camera = SimpleNamespace(component=SimpleNamespace(path='path' if path else None))
    if lod is not None:
        job.settings.lod = lod
    if shaping is not None:
        job.settings.fit = shaping
    start_path(work, job).write_bytes(data)
    job.renderer.variant = SimpleNamespace(name='fixture')
    job.renderer.fit = SimpleNamespace(steps=1, batch=1, laplacian=0, normal_weight=0,
                                       surface_weight=0, surface_margin=0, budget=1)
    target = work / (job.asset_name + mesh_bundle.SUFFIX)
    with mock.patch.dict(sys.modules, {'r3d.appearance_simplify': SimpleNamespace(main=fit_main)}):
        stage.fit(work / 'scene.scene.toml', scene, job, work, start=start_path(work, job), target=target)
    return data, job, target


class FitCommandTests(unittest.TestCase):
    """The command line the fit stage hands the fit: which sets, in which order, and the settle."""
    SETTLE = SimpleNamespace(step=100, settle=0.05)

    def command(self, path=True, probes=None, budget=None):
        asked = []
        shaping = SimpleNamespace(probes=probes, budget=budget)
        with tempfile.TemporaryDirectory() as directory:
            run_fit(directory, lambda args: asked.append(args) or keep_start(args), path=path, shaping=shaping)
            work = str(pathlib.Path(directory))
        return [pathlib.Path(arg).name if arg.startswith(work) else arg for arg in asked[0]]

    @staticmethod
    def values(args, flag):
        return [args[i + 1] for i, arg in enumerate(args) if arg == flag]

    def test_the_path_sets_come_before_the_probe_sets(self):
        args = self.command(probes=SimpleNamespace())
        self.assertEqual(self.values(args, '--poses'), ['train.txt', 'train_landscape.txt', 'probes.txt'])
        self.assertEqual(self.values(args, '--reference'), ['reference', 'reference_landscape', 'reference_probes'])
        self.assertEqual(self.values(args, '--coverage-poses'), ['coverage.txt', 'probes.txt'])

    def test_a_camera_without_a_path_fits_on_the_probe_sets_alone(self):
        args = self.command(path=False, probes=SimpleNamespace())
        self.assertEqual(self.values(args, '--poses'), ['probes.txt'])
        self.assertEqual(self.values(args, '--coverage-poses'), ['probes.txt'])
        self.assertEqual(self.values(args, '--held-out-poses'), ['probes_held_out.txt'])

    def test_without_probes_the_fit_reads_the_path_sets_alone(self):
        args = self.command()
        self.assertEqual(self.values(args, '--poses'), ['train.txt', 'train_landscape.txt'])
        self.assertEqual(self.values(args, '--coverage-poses'), ['coverage.txt'])

    def test_held_out_sets_pass_only_when_the_import_sets_probes_or_a_budget_and_pair_up(self):
        for name, shaping in (('none', {}), ('probes', dict(probes=SimpleNamespace())),
                              ('budget', dict(budget=self.SETTLE))):
            with self.subTest(shaping=name):
                args = self.command(**shaping)
                held_out = ['held_out.txt'] + (['probes_held_out.txt'] if name == 'probes' else [])
                references = ['reference_held_out'] + (['reference_probes_held_out'] if name == 'probes' else [])
                if name == 'none':
                    self.assertNotIn('--held-out-poses', args)
                    self.assertNotIn('--held-out-reference', args)
                else:
                    self.assertEqual(self.values(args, '--held-out-poses'), held_out)
                    self.assertEqual(self.values(args, '--held-out-reference'), references)

    def test_settle_is_passed_only_with_a_budget(self):
        args = self.command(budget=self.SETTLE)
        self.assertEqual(self.values(args, '--settle'), ['100'])
        self.assertEqual(args[args.index('--settle') + 2], '0.05')
        self.assertNotIn('--settle', self.command())

    def test_the_copy_gate_checks_the_path_held_out_set_unless_the_camera_has_no_path(self):
        for path, probes, expected in ((True, SimpleNamespace(), 'held_out.txt'), (True, None, 'held_out.txt'),
                                       (False, SimpleNamespace(), 'probes_held_out.txt')):
            with self.subTest(path=path, probes=probes):
                self.assertEqual(self.values(self.command(path, probes), '--copy-poses'), [expected])


