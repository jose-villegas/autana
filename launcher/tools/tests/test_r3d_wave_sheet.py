"""Checks the wave sheet (r3d/procedural/wave_sheet.py): its triangles follow the formula and face their side,
two skins close into one shell, a colour edge opens no crack, and its source reads back through the OBJ loader.
It also checks the import of a closed double-sided shell: it collapses to one sheet halfway between its skins
(r3d/source/midsurface.py), the far skin gone but still what the sheet shows from behind, and on CUDA the fitted
cloth then shows no far skin, where kept whole it does. Needs the pinned r3d environment
(tools/r3d/requirements.txt) with LLVM for the import; the fit needs CUDA, PyTorch and nvdiffrast."""

import collections
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from tests.r3d_env import needs_llvm  # noqa: E402

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
# shallow enough that the sweep's tilted views never see one skin from behind.
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
    from r3d.geometry import face_normals

    return face_normals(sheet.positions, sheet.tris)[0]


def welded_tris(sheet):
    _, point = np.unique(sheet.positions, axis=0, return_inverse=True)
    return point.reshape(-1)[sheet.tris]


def directed_edges(tris):
    return collections.Counter((int(a), int(b)) for t in tris for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])))


def write_import(directory, sheet, name, double_sided, triangles=CLOTH_TRIANGLES):
    """`sheet` as an import listing `double_sided` materials, simplified to `triangles`; its path."""
    root = pathlib.Path(directory)
    wave_sheet.write_source(sheet, root, name)
    listed = ", ".join(f'"{material}"' for material in double_sided)
    path = root / f"{name}.import.toml"
    path.write_text(
        f'[source]\npath="{name}.obj"\ncredit="test"\n[output]\nposition_scale=8\n'
        f"[materials]\ndouble_sided=[{listed}]\n"
        '[geometry]\nsimplify={dense_edge=100.0, colour_deviation=1.0, props=[], props_share=0.0, seal_seams=false}\n'
        f'[[variants]]\nname="{name}"\ntriangles={triangles}\n')
    return path


def imported(sheet, double_sided, scene=True, triangles=CLOTH_TRIANGLES):
    """The geometry the import makes of `sheet`, simplified to `triangles`: placed in a scene whose camera region
    is REGION, or with `scene` False as a bare import, which has no camera."""
    from r3d import mesh_import
    from r3d.import_settings import albedo_jobs, load_import_settings, load_scene
    from test_r3d_import import renderer, write_scene

    with tempfile.TemporaryDirectory() as directory:
        path = write_import(directory, sheet, "cloth", double_sided, triangles)
        if not scene:
            return mesh_import.bake_geometry(albedo_jobs(load_import_settings(path))[0], None)
        camera = '[[objects]]\nname = "camera"\n[objects.camera]\nhalf_fov_short_tan = 0.6\nnear_z = 1.0\n' + REGION
        objects = renderer("cloth.import.toml", 'variant = "cloth"\n') + camera
        loaded = load_scene(write_scene(directory, objects))
        return mesh_import.bake_geometry(loaded.renderers[0], loaded)


def facing(geometry):
    """The z of each imported triangle's normal."""
    from r3d.geometry import face_normals

    return face_normals(geometry.positions, geometry.tris)[0][:, 2]


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
        # Three triangles on one place share all their edges, each used three times: open, not closed.
        stack = np.array([[0, 1, 2], [0, 1, 2], [0, 2, 1]])
        self.assertTrue((closed_shells(two.positions, stack) == -1).all(), "coincident open triangles closed a shell")

    def test_the_far_skin_is_the_one_facing_away_from_the_viewer_whatever_the_wave(self):
        from r3d.mesh_import import far_skins

        # Above, below, and off to one side: the viewer stands where a camera region over cloth would.
        for shape in ("sine", "zigzag", "sawtooth"):
            cloth = wave_sheet.sheet(CLOTH_COUNT, layers=2, thickness=CLOTH_THICKNESS, **dict(CLOTH, shape=shape))
            skins, every = cloth.layer != wave_sheet.HEM, np.ones(len(cloth.tris), dtype=bool)
            for towards, far_layer in (((0.0, 0.0, 200.0), wave_sheet.BACK), ((0.0, 0.0, -200.0), wave_sheet.FRONT),
                                       ((60.0, -40.0, 200.0), wave_sheet.BACK)):
                far = far_skins(cloth.positions, cloth.tris, every, np.array(towards))
                with self.subTest(shape=shape, towards=towards):
                    self.assertTrue(np.array_equal(far[skins], cloth.layer[skins] == far_layer))
        # A triangle with no area faces nowhere: it is never taken for the far skin.
        flat = np.concatenate([cloth.tris, [[0, 0, 1]]])
        far = far_skins(cloth.positions, flat, np.ones(len(flat), dtype=bool), np.array((0.0, 0.0, 200.0)))
        self.assertFalse(far[-1])

    @needs_llvm
    def test_the_import_collapses_a_closed_shell_to_one_double_sided_sheet_halfway_between_its_skins(self):
        from r3d.geometry import nearest_triangles, triangle_search

        cloth = wave_sheet.sheet(CLOTH_COUNT, layers=2, thickness=CLOTH_THICKNESS, **CLOTH)
        front = every_material(cloth)[:-1]  # the far skin's own colour is not listed: the shell is still closed
        placed = imported(cloth, front)
        self.assertTrue(len(placed.tris))
        self.assertFalse((facing(placed) < -SIDE_COS).any(), "a triangle of the far skin is left")
        listed = np.isin(placed.tri_mat, [placed.src.names.index(name) for name in front])
        self.assertTrue(placed.tri_double[listed].all(), "the sheet is not double-sided")
        # Each vertex of the sheet lies as far from one skin as from the other, before the simplifier, which
        # follows the folds only as far as its budget lets it, moves any.
        whole = imported(cloth, front, triangles=len(cloth.tris))
        skin = {layer: triangle_search(cloth.positions[cloth.tris[cloth.layer == layer]])
                for layer in (wave_sheet.FRONT, wave_sheet.BACK)}
        gap = {layer: nearest_triangles(whole.positions, search)[2] for layer, search in skin.items()}
        # Exactly halfway off the creases; at a crease the ray along the averaged normal is longer than the
        # thickness, and its midpoint stands nearer the inside of the fold.
        for layer in (wave_sheet.FRONT, wave_sheet.BACK):
            off = np.abs(gap[layer] - CLOTH_THICKNESS / 2)
            self.assertLess(np.median(off), CLOTH_THICKNESS / 100)
            self.assertLess(off.max(), CLOTH_THICKNESS / 4)
        # The rim goes and the whole near skin stays: the sheet covers the cloth.
        self.assertEqual(len(whole.tris), int((cloth.layer == wave_sheet.FRONT).sum()))
        # From behind it shows the far skin's colour, unlit: an import draws albedo.
        self.assertEqual(placed.rgb.shape[1], 6)
        double_vertices = np.unique(placed.tris[listed])
        back = np.median(placed.rgb[double_vertices, 3:], axis=0)
        np.testing.assert_allclose(back, wave_sheet.BACK_COLOUR, atol=2)
        bare = imported(cloth, front, scene=False)
        self.assertTrue((facing(bare) < -SIDE_COS).any(), "with no camera to face, both skins are kept")

    @needs_llvm
    def test_the_far_skin_leaves_the_light_too_where_the_sheet_casts_it(self):
        from unittest import mock

        from r3d import mesh_import
        from r3d.import_settings import albedo_jobs, load_import_settings
        from r3d.ray_query import RayQuery

        cloth = wave_sheet.sheet(CLOTH_COUNT, layers=2, thickness=CLOTH_THICKNESS, **CLOTH)
        built, shaded = [], []

        def ray_query(points, tris, *rest, **options):
            built.append(len(tris))
            query = RayQuery(points, tris, *rest, **options)
            query.name = f"query of {len(tris)}"
            return query

        def shade_lit(src, job, scene, material, mp, mt, double, intersector, bounce, *, labels, **options):
            shaded.append(intersector)
            return (*mesh_import.shade_unlit(src, material, mp, mt), labels)

        with tempfile.TemporaryDirectory() as directory:
            job = albedo_jobs(load_import_settings(write_import(directory, cloth, "cloth", every_material(cloth))))[0]
            job = SimpleNamespace(settings=job.settings, renderer=job.renderer,
                                  bake=SimpleNamespace(ray_offset=0.0, ao=None, indirect=None))
            scene = SimpleNamespace(lights=[], indirect=SimpleNamespace(intensity=1.0), tonemap_white=1.0,
                                    region=np.array([[-50.0, -50.0, 150.0], [50.0, 50.0, 250.0]]))
            with mock.patch.object(mesh_import, "RayQuery", side_effect=ray_query),                     mock.patch.object(mesh_import, "shade_lit", side_effect=shade_lit),                     mock.patch.object(mesh_import, "light", side_effect=lambda p, *rest, **kw: np.ones((len(p), 3))),                     mock.patch.object(mesh_import, "path_light_for", side_effect=lambda *rest: None):
                geometry = mesh_import.bake_geometry(job, scene)
        far = int((cloth.layer == wave_sheet.BACK).sum())
        drawn = len(geometry.src.tri_v)
        self.assertLessEqual(drawn, len(cloth.tris) - far, "the far skin is still drawn")
        self.assertTrue(shaded and all(query is geometry.intersector for query in shaded))
        self.assertEqual(geometry.intersector.name, f"query of {drawn}", "the light meets more than the sheet")

    def test_the_import_keeps_an_open_double_sided_sheet_whole(self):
        sheet = wave_sheet.sheet(CLOTH_COUNT, **CLOTH)
        placed, bare = imported(sheet, every_material(sheet)), imported(sheet, every_material(sheet), scene=False)
        self.assertEqual(len(placed.tris), len(bare.tris))
        self.assertTrue(placed.tri_double.all())


@unittest.skipIf(np is None or not GPU, "needs the r3d environment, CUDA, PyTorch and nvdiffrast")
class FittedClothTests(unittest.TestCase):
    """A two-skin cloth through the import and the fit: its far skin must never show through the near one, as it
    did on Sponza's drapes while both skins of their closed fabric were kept. The cloth's near skin faces every
    training view, so a pixel showing a triangle from behind shows the far skin. Seen from below, the collapsed
    sheet shows its back colours, read off the far skin: no worse than keeping both skins."""

    def fitted_measures(self, scene):
        """The held-out measures of the cloth imported, with a camera to face (`scene`) or bare so that both
        skins are kept, written as the import draws it and fitted to the cloth as it is: a closed shell; and
        below_de76, its mean dE76 over held-out views from under it."""
        from r3d.appearance_simplify import delta_e_total, optimise, start_mesh
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
        fitted = mesh._replace(points=points, rgb=rgb)
        below = wave_sweep.references(cloth, False, wave_sweep.views(wave_sweep.HELD_OUT_VIEWS, 0.5, below=True),
                                      "cuda")
        total, pixels = delta_e_total(fitted, below, wave_sweep.VIEW_SIZE, wave_sweep.CLEAR)
        return {**wave_sweep.picture_measures(fitted, held_out, "cuda"), "below_de76": total / pixels}

    def test_an_imported_and_fitted_cloth_shows_no_far_skin_where_kept_whole_it_would(self):
        placed = self.fitted_measures(True)
        self.assertEqual(placed["back_px"], 0, "the fitted cloth shows its far skin through its near one")
        whole = self.fitted_measures(False)
        self.assertGreater(whole["back_px"], 0, "the cloth is too gentle for its far skin to show when kept")
        self.assertLessEqual(placed["holes"], whole["holes"], "dropping the far skin opened holes")
        self.assertLessEqual(placed["below_de76"], whole["below_de76"],
                             "seen from behind, the sheet is further from the cloth than its two skins were")


if __name__ == "__main__":
    unittest.main()
