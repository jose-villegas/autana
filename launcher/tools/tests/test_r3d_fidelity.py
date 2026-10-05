"""Checks the flat-bake knobs bake_fidelity.py sweeps: where a face's samples
sit, how many rays the sky tests, the hardness of the sun's shadow edge, and the variant spec."""

import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    from tests import soup

    from r3d import mesh_import
    from r3d.bake_fidelity import write_variant
    from r3d.bake_fidelity import parse_spec
    from r3d.import_settings import SettingsError
    from r3d.light import face_samples, light
except ImportError:
    np = None

from test_r3d_import import AMBIENT, CUBE, TONEMAP, renderer, sun_object, write_import, write_scene  # noqa: E402

FLAT_BAKE = '[bake]\nray_offset = 0.5\ncolour_merge_step = 6\n'
FLAT_VARIANT = '[[variants]]\nname = "mesh"\n'
FLAT_RENDERER = 'variant = "mesh"\nbake = true\nshading = { flat = { auto = { min = 1, max = 8, area = 8.0 } } }\n'
# A small triangle, and a roof over half of the cube's top face, so samples within one face disagree.
SMALL_FACE = ("v 20 0 0\nv 21 0 0\nv 20 1 0\nf 9 10 11\n"
              "v -1 12 -1\nv 4 12 -1\nv 4 12 9\nv -1 12 9\nf 12 13 14\nf 12 14 15\n")


def sun_only():
    return [{"type": "directional", "direction": [0.8, 1.0, 0.0], "color": [1.0, 1.0, 1.0], "intensity": 1.0}]


@unittest.skipIf(np is None, "the r3d environment is not installed")
class PlacementTests(unittest.TestCase):
    def test_centroid_samples_all_sit_at_the_centroid(self):
        np.testing.assert_allclose(face_samples(4, "centroid"), np.full((4, 3), 1.0 / 3.0))

    def test_stratified_samples_spread_over_the_face(self):
        self.assertGreater(np.ptp(face_samples(4)[:, 1]), 0.1)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SunTests(unittest.TestCase):
    def radiance_at(self, x):
        floor = soup.Soup([(0, 0, 0), (0, 0, 8), (16, 0, 8), (16, 0, 0)], [(0, 1, 2), (0, 2, 3)])
        wall = soup.box(extents=(1, 6, 8))
        wall.apply_translation((8.5, 3, 4))
        point, up = np.array([[x, 0.0, 4.0]]), np.array([[0.0, 1.0, 0.0]])
        return light(point, up, np.array([False]), soup.rays(soup.concatenate([floor, wall])), sun_only(), 0.01)[0, 0]

    def test_the_sun_is_a_point_so_a_shadow_edge_is_hard(self):
        # The wall's top edge shades the floor from x = 3.2 on: just before it the sun lights fully, just after, not at all.
        self.assertAlmostEqual(self.radiance_at(3.0), 1.0 / np.hypot(0.8, 1.0), places=9)
        self.assertEqual(self.radiance_at(3.4), 0.0)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SkyRaysKnobTests(unittest.TestCase):
    def test_sky_rays_replaces_the_sky_lights_ray_count_only_and_leaves_the_scene_alone(self):
        from types import SimpleNamespace
        lights = [sun_only()[0], {"type": "sky", "color": [1.0, 1.0, 1.0], "intensity": 1.0, "rays": 48},
                  {"type": "ambient", "color": [1.0, 1.0, 1.0], "intensity": 0.1}]
        scene = SimpleNamespace(lights=lights, tonemap_white=0.35)
        job = SimpleNamespace(settings=SimpleNamespace(double_sided=set()), bake=SimpleNamespace(ray_offset=0.5, ao=None))
        geometry = SimpleNamespace(src=SimpleNamespace(names=["m"]), positions=None, tris=None, tri_mat=None, intersector=None,
                                   bounce=None)
        with mock.patch.object(mesh_import, "face_colours", return_value="colours") as face:
            self.assertEqual(mesh_import.flat_colours(job, scene, geometry, (1, 1, 1, None), sky_rays=7), "colours")
        passed = face.call_args.args[7]
        self.assertEqual([item.get("rays") for item in passed], [None, 7, None])
        self.assertEqual(scene.lights[1]["rays"], 48)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SpecTests(unittest.TestCase):
    def test_an_area_is_a_multiple_of_the_median(self):
        samples, knobs = parse_spec("samples=auto:2:8:median*0.5,sky=64,place=centroid", None, 10.0)
        self.assertEqual(samples, ("auto", 2, 8, 5.0))
        self.assertEqual(knobs, {"sky_rays": 64, "placement": "centroid"})

    def test_a_fixed_count_and_a_missing_spec_keep_the_declared_options(self):
        self.assertEqual(parse_spec("samples=fixed:4", None, 1.0)[0], (4, 1, 4, None))
        self.assertEqual(parse_spec("", ("auto", 1, 16, 3.0), 1.0), (("auto", 1, 16, 3.0), {}))

    def test_an_unknown_key_is_refused(self):
        with self.assertRaises(SettingsError):
            parse_spec("sky=64,colour=red", None, 1.0)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class DeclaredVariantTests(unittest.TestCase):
    def test_the_declared_variant_is_the_bake_the_importer_writes_byte_for_byte(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "m.obj").write_text(CUBE + SMALL_FACE)
            (root / "m.mtl").write_text("newmtl m\nKd 0.5 0.25 0.125\n")
            write_import(root, output='[output]\ndirectory = "."\n', body=FLAT_VARIANT)
            scene_path = write_scene(root, renderer(extra=FLAT_RENDERER) + sun_object(), head=TONEMAP + AMBIENT + FLAT_BAKE,
                                     name="mesh.scene.toml")
            with mock.patch("r3d.mesh_import.REPO", root):
                self.assertEqual(mesh_import.main([str(scene_path)]), 0)
                scene = mesh_import.load_scene(scene_path)
                job = scene.renderers[0]
                geometry = mesh_import.bake_geometry(job, scene)
                written = write_variant(job, scene, geometry, "", root / "scratch")
            tracked = (root / "mesh.mesh.mesh").read_bytes()
            self.assertEqual(written.read_bytes(), tracked)
            self.assertGreater(len(set(np.unique(geometry.tri_mat))), 0)

    def test_a_different_sample_count_changes_the_bake(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "m.obj").write_text(CUBE + SMALL_FACE)
            (root / "m.mtl").write_text("newmtl m\nKd 0.5 0.25 0.125\n")
            write_import(root, output='[output]\ndirectory = "."\n', body=FLAT_VARIANT)
            scene_path = write_scene(root, renderer(extra=FLAT_RENDERER) + sun_object(), head=TONEMAP + AMBIENT + FLAT_BAKE,
                                     name="mesh.scene.toml")
            with mock.patch("r3d.mesh_import.REPO", root):
                scene = mesh_import.load_scene(scene_path)
                job = scene.renderers[0]
                geometry = mesh_import.bake_geometry(job, scene)
                declared = write_variant(job, scene, geometry, "", root / "a")
                other = write_variant(job, scene, geometry, "samples=fixed:1,place=centroid",
                                      root / "b")
            self.assertNotEqual(declared.read_bytes(), other.read_bytes())


if __name__ == "__main__":
    unittest.main()
