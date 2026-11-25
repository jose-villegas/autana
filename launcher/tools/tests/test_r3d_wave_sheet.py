"""Checks the wave sheet (r3d/procedural/wave_sheet.py): its triangles follow the formula and face their side,
two skins close into one shell, a colour edge opens no crack, and its source reads back through the OBJ loader.
It also checks the import of a closed double-sided shell: the skin the camera region does not face leaves it,
and on CUDA the fitted cloth then shows no far skin, where kept whole it does. Needs the pinned r3d
environment (tools/r3d/requirements.txt); the fit needs CUDA, PyTorch and nvdiffrast."""

import collections
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.procedural import wave_sheet
except ImportError:
    np = None

try:
    import torch
    import nvdiffrast.torch  # noqa: F401

    GPU = torch.cuda.is_available()
except ImportError:
    GPU = False

# The cloth the import and fit checks build: a zigzag tight enough that its coarsened skins cross, its slopes
# (39 degrees) shallow enough that the sweep's views, tilted up to 40 degrees, never see one skin from behind.
CLOTH = dict(size=100.0, shape="zigzag", amplitude=5.0, frequency=(4, 0), pattern="checker", cells=4)
CLOTH_COUNT = 32
CLOTH_THICKNESS = 1.2
CLOTH_TRIANGLES = 300  # the import's budget: coarse, as a scene's fabric is
FIT_STEPS = 100
# A triangle facing within this cosine of +z or -z belongs to a skin, not to the hem between them.
SIDE_COS = 0.5
# A camera region above the cloth, which faces +z: the skin it faces is the front.
REGION = "region = { min = [-50.0, -50.0, 150.0], max = [50.0, 50.0, 250.0] }\n"


def face_normals(sheet):
    corners = sheet.positions[sheet.tris]
    normals = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    return normals / np.linalg.norm(normals, axis=1, keepdims=True)


def welded_tris(sheet):
    _, point = np.unique(sheet.positions, axis=0, return_inverse=True)
    return point.reshape(-1)[sheet.tris]


def directed_edges(tris):
    return collections.Counter((int(a), int(b)) for t in tris for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])))


def write_import(directory, sheet, name, double_sided):
    """`sheet` as an import listing `double_sided` materials; its path."""
    root = pathlib.Path(directory)
    wave_sheet.write_source(sheet, root, name)
    listed = ", ".join(f'"{material}"' for material in double_sided)
    path = root / f"{name}.import.toml"
    path.write_text(
        f'[source]\npath="{name}.obj"\ncredit="test"\n[output]\nposition_scale=8\n'
        f"[materials]\ndouble_sided=[{listed}]\n"
        '[geometry]\nsimplify={dense_edge=100.0, colour_deviation=1.0, props=[], props_share=0.0, seal_seams=false}\n'
        f'[[variants]]\nname="{name}"\ntriangles={CLOTH_TRIANGLES}\n')
    return path


def imported(sheet, double_sided, scene=True):
    """The geometry the import makes of `sheet`: placed in a scene whose camera region is REGION, or with
    `scene` False as a bare import, which has no camera."""
    from r3d import mesh_import
    from r3d.import_settings import albedo_jobs, load_import_settings, load_scene
    from test_r3d_import import renderer, write_scene

    with tempfile.TemporaryDirectory() as directory:
        path = write_import(directory, sheet, "cloth", double_sided)
        if not scene:
            return mesh_import.bake_geometry(albedo_jobs(load_import_settings(path))[0], None)
        camera = '[[objects]]\nname = "camera"\n[objects.camera]\nhalf_fov_short_tan = 0.6\nnear_z = 1.0\n' + REGION
        objects = renderer("cloth.import.toml", 'variant = "cloth"\n') + camera
        loaded = load_scene(write_scene(directory, objects))
        return mesh_import.bake_geometry(loaded.renderers[0], loaded)


def facing(geometry):
    """The z of each imported triangle's normal."""
    corners = geometry.positions[geometry.tris]
    normals = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    return normals[:, 2] / np.linalg.norm(normals, axis=1)


def every_material(sheet):
    return [wave_sheet.material_name(m) for m in range(len(sheet.colours)) if (sheet.material == m).any()]


@unittest.skipIf(np is None, "the r3d environment is not installed")
class WaveSheetTests(unittest.TestCase):
    def test_triangles_follow_the_formula_off_its_creases_and_closer_as_quads_shrink(self):
        for shape in ("sine", "zigzag", "sawtooth"):
            worst = []
            for count in (16, 64):
                sheet = wave_sheet.sheet(count, shape=shape, amplitude=0.1, frequency=(3, 0))
                centres = sheet.positions[sheet.tris].mean(axis=1)
                formula = sheet.surface.normal(centres[:, 0], centres[:, 1])
                plain = ~sheet.kink[sheet.tris].any(axis=1)
                cosine = np.clip((face_normals(sheet) * formula).sum(axis=1)[plain], -1, 1)
                worst.append(np.degrees(np.arccos(cosine)).max())
            with self.subTest(shape=shape):
                if shape == "sine":
                    self.assertLess(worst[1], worst[0] / 2, "a finer sine did not follow its curve closer")
                else:
                    self.assertLess(worst[1], 1e-6, "a straight piece is not flat between its creases")

    def test_kinks_mark_the_vertices_beside_creases_and_drops_only(self):
        self.assertFalse(wave_sheet.sheet(32, shape="sine", amplitude=0.1, frequency=(3, 0)).kink.any())
        for shape, per_cycle in (("zigzag", 2), ("sawtooth", 1)):
            sheet = wave_sheet.sheet(32, shape=shape, amplitude=0.1, frequency=(4, 0))
            columns = np.unique(np.round(sheet.positions[sheet.kink, 0], 9))
            with self.subTest(shape=shape):
                self.assertGreaterEqual(len(columns), 4 * per_cycle)
                self.assertLessEqual(len(columns), 2 * 4 * per_cycle + 2, "a kink lies away from any crease")

    def test_each_skin_faces_its_side_and_two_skins_close_into_one_shell(self):
        one = wave_sheet.sheet(16, shape="zigzag", amplitude=0.2, frequency=(2, 0))
        self.assertTrue((face_normals(one)[:, 2] > 0).all(), "a front triangle faces away")
        self.assertTrue(any(count == 1 and (b, a) not in directed_edges(welded_tris(one))
                            for (a, b), count in directed_edges(welded_tris(one)).items()), "one skin has no border")
        two = wave_sheet.sheet(16, shape="zigzag", amplitude=0.2, frequency=(2, 0), layers=2, thickness=0.02)
        normals = face_normals(two)
        self.assertTrue((normals[two.layer == wave_sheet.FRONT, 2] > 0).all())
        self.assertTrue((normals[two.layer == wave_sheet.BACK, 2] < 0).all(), "a back triangle faces the front")
        edges = directed_edges(welded_tris(two))
        self.assertTrue(all(edges[(b, a)] == count for (a, b), count in edges.items()),
                        "the shell is open or turned inside out somewhere")
        self.assertEqual(two.colours[-1], wave_sheet.BACK_COLOUR)

    def test_a_colour_edge_duplicates_its_vertices_and_opens_no_crack(self):
        count = 8
        sheet = wave_sheet.sheet(count, shape="sine", amplitude=0.1, frequency=(2, 1), pattern="checker", cells=4)
        self.assertEqual(len(set(map(tuple, sheet.colours))), len(sheet.colours))
        vertex_colours = collections.defaultdict(set)
        for t, m in zip(sheet.tris, sheet.material):
            for v in t:
                vertex_colours[int(v)].add(int(m))
        self.assertTrue(all(len(colours) == 1 for colours in vertex_colours.values()), "a vertex carries two colours")
        self.assertGreater(len(sheet.positions), (count + 1) ** 2, "no colour edge was split")
        edges = directed_edges(welded_tris(sheet))
        border = [edge for edge in edges if (edge[1], edge[0]) not in edges]
        self.assertEqual(len(border), 4 * count, "a colour edge opened a crack inside the sheet")

    def test_the_source_reads_back_through_the_obj_loader(self):
        from r3d.obj import load_mtl, load_obj

        sheet = wave_sheet.sheet(6, shape="sawtooth", amplitude=0.2, frequency=(2, 0), pattern="stripes", cells=3,
                                 layers=2, thickness=0.05)
        with tempfile.TemporaryDirectory() as directory:
            path = wave_sheet.write_source(sheet, directory, "sheet")
            p, _uv, tri_v, _tri_t, tri_m, names, _tri_o, _objects = load_obj(path)
            materials = load_mtl(path.with_suffix(".mtl"))
        self.assertEqual(len(tri_v), len(sheet.tris))
        self.assertEqual(sorted(names), sorted(every_material(sheet)))
        read = p[tri_v].reshape(-1, 3)
        written = sheet.positions[sheet.tris].reshape(-1, 3)
        gap = np.linalg.norm(read[:, None] - written[None], axis=-1)
        self.assertLess(gap.min(axis=1).max(), 1e-5, "a corner read back lies off the sheet")
        self.assertLess(gap.min(axis=0).max(), 1e-5, "a corner of the sheet was not written")
        for name in names:
            colour = sheet.colours[int(name.split("_")[1])]
            self.assertTrue(np.allclose(materials[name]["Kd"], np.array(colour) / 255.0, atol=1e-6))

    def test_closed_shells_name_each_closed_piece_whatever_its_materials_and_none_for_an_open_one(self):
        from r3d.geometry import closed_shells

        two = wave_sheet.sheet(8, shape="zigzag", amplitude=0.2, frequency=(2, 0), layers=2, thickness=0.02)
        one = wave_sheet.sheet(8, shape="zigzag", amplitude=0.2, frequency=(2, 0), centre=(3.0, 0.0))
        self.assertGreater(len(set(two.material.tolist())), 1)
        piece = closed_shells(np.concatenate([two.positions, one.positions]),
                              np.concatenate([two.tris, one.tris + len(two.positions)]))
        self.assertEqual(len(set(piece[:len(two.tris)].tolist())), 1, "one shell of many materials was split")
        self.assertTrue((piece[:len(two.tris)] >= 0).all())
        self.assertTrue((piece[len(two.tris):] == -1).all(), "an open sheet was taken for a closed shell")
        # A fin standing on one of the shell's edges, as cloth touches the rod it hangs from: that edge is used
        # three times, and the shell stays closed while the fin, all border, does not.
        points = np.concatenate([two.positions, [two.positions[two.tris[0, 0]] + (0.0, 0.0, 1.0)]])
        fin = [two.tris[0, 0], two.tris[0, 1], len(two.positions)]
        piece = closed_shells(points, np.concatenate([two.tris, [fin]]))
        self.assertTrue((piece[:-1] >= 0).all(), "a shell touched along an edge was taken for an open one")
        self.assertEqual(piece[-1], -1)

    def test_the_far_skin_is_the_one_facing_away_from_the_viewer(self):
        from r3d.mesh_import import far_skins

        cloth = wave_sheet.sheet(CLOTH_COUNT, layers=2, thickness=CLOTH_THICKNESS, **CLOTH)
        skins = cloth.layer != wave_sheet.HEM
        for towards, far_layer in (((0.0, 0.0, 200.0), wave_sheet.BACK), ((0.0, 0.0, -200.0), wave_sheet.FRONT)):
            far = far_skins(cloth.positions, cloth.tris, np.ones(len(cloth.tris), dtype=bool), np.array(towards))
            with self.subTest(towards=towards):
                self.assertTrue(np.array_equal(far[skins], cloth.layer[skins] == far_layer))

    def test_the_import_keeps_the_skin_a_camera_faces_of_a_closed_shell_and_draws_it_double_sided(self):
        cloth = wave_sheet.sheet(CLOTH_COUNT, layers=2, thickness=CLOTH_THICKNESS, **CLOTH)
        front = every_material(cloth)[:-1]  # the far skin's own colour is not listed: the shell is still closed
        placed = imported(cloth, front)
        self.assertTrue(len(placed.tris))
        self.assertFalse((facing(placed) < -SIDE_COS).any(), "a triangle of the far skin is left")
        listed = np.isin(placed.tri_mat, [placed.src.names.index(name) for name in front])
        self.assertTrue(placed.tri_double[listed].all(), "the near skin is not double-sided")
        bare = imported(cloth, front, scene=False)
        self.assertTrue((facing(bare) < -SIDE_COS).any(), "with no camera to face, both skins are kept")

    def test_the_import_keeps_an_open_double_sided_sheet_whole(self):
        sheet = wave_sheet.sheet(CLOTH_COUNT, **CLOTH)
        placed, bare = imported(sheet, every_material(sheet)), imported(sheet, every_material(sheet), scene=False)
        self.assertEqual(len(placed.tris), len(bare.tris))
        self.assertTrue(placed.tri_double.all())


@unittest.skipIf(np is None or not GPU, "needs the r3d environment, CUDA, PyTorch and nvdiffrast")
class FittedClothTests(unittest.TestCase):
    """A two-skin cloth through the import and the fit: its far skin must never show through the near one, as it
    did on Sponza's drapes while both skins of their closed fabric were kept. The cloth's near skin faces every
    view, so a pixel showing a triangle from behind shows the far skin."""

    def fitted_measures(self, scene):
        """The held-out measures of the cloth imported, with a camera to face (`scene`) or bare so that both
        skins are kept, written as the import draws it and fitted to the cloth as it is: a closed shell."""
        from r3d.appearance_simplify import optimise, start_mesh
        from r3d.fit import wave_sweep
        from r3d.lit_mesh import write_lit_mesh

        cloth = wave_sheet.sheet(CLOTH_COUNT, layers=2, thickness=CLOTH_THICKNESS, **CLOTH)
        geometry = imported(cloth, every_material(cloth), scene)
        with tempfile.TemporaryDirectory() as directory:
            write_lit_mesh(directory, "start", geometry.positions, geometry.rgb, geometry.tris, geometry.tri_double,
                           position_scale=wave_sweep.POSITION_SCALE)
            mesh = start_mesh(pathlib.Path(directory) / "start.bundle")
        train = wave_sweep.references(cloth, False, wave_sweep.views(wave_sweep.TRAIN_VIEWS, 0.0), "cuda")
        held_out = wave_sweep.references(cloth, False, wave_sweep.views(wave_sweep.HELD_OUT_VIEWS, 0.5), "cuda")
        points, rgb, _history = optimise(mesh, train, wave_sweep.VIEW_SIZE, report=0, device="cuda",
                                         clear=wave_sweep.CLEAR,
                                         source=SimpleNamespace(p=cloth.positions, tri_v=cloth.tris),
                                         **dict(wave_sweep.FIT_DEFAULTS, steps=FIT_STEPS))
        return wave_sweep.picture_measures(mesh._replace(points=points, rgb=rgb), held_out, "cuda")

    def test_an_imported_and_fitted_cloth_shows_no_far_skin_where_kept_whole_it_would(self):
        placed = self.fitted_measures(True)
        self.assertEqual(placed["back_px"], 0, "the fitted cloth shows its far skin through its near one")
        whole = self.fitted_measures(False)
        self.assertGreater(whole["back_px"], 0, "the cloth is too gentle for its far skin to show when kept")
        self.assertLessEqual(placed["holes"], whole["holes"], "dropping the far skin opened holes")


if __name__ == "__main__":
    unittest.main()
