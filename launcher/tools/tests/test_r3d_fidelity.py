"""Checks the flat-bake knobs bake_fidelity.py sweeps: where a face's samples
sit, whether the sun is its whole disc or its middle, and the variant spec."""

import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    import trimesh
    from trimesh.ray.ray_pyembree import RayMeshIntersector

    from r3d import mesh_import
    from r3d.bake_fidelity import write_variant
    from r3d.bake_fidelity import parse_spec
    from r3d.import_settings import SettingsError
    from r3d.light import face_samples, light
except ImportError:
    np = None

from test_r3d_import import AMBIENT, CUBE, TONEMAP, renderer, sun_object, write_import, write_scene  # noqa: E402

FLAT_BAKE = '[bake]\nray_offset = 0.5\ncolour_merge_step = 6\nflat_sky_rays = 16\n'
FLAT_VARIANT = '[[variants]]\nname = "mesh"\n'
FLAT_RENDERER = 'variant = "mesh"\nbake = true\nshading = { flat = { auto = { min = 1, max = 8, area = 8.0 } } }\n'
# A small triangle, and a roof over half of the cube's top face, so samples within one face disagree.
SMALL_FACE = ("v 20 0 0\nv 21 0 0\nv 20 1 0\nf 9 10 11\n"
              "v -1 12 -1\nv 4 12 -1\nv 4 12 9\nv -1 12 9\nf 12 13 14\nf 12 14 15\n")


def sun_only(disc_degrees):
    return [{"type": "directional", "direction": [0.8, 1.0, 0.0], "color": [1.0, 1.0, 1.0], "intensity": 1.0,
             "disc_degrees": disc_degrees, "rays": 8}]


@unittest.skipIf(np is None, "the r3d environment is not installed")
class PlacementTests(unittest.TestCase):
    def test_centroid_samples_all_sit_at_the_centroid(self):
        np.testing.assert_allclose(face_samples(4, "centroid"), np.full((4, 3), 1.0 / 3.0))

    def test_stratified_samples_spread_over_the_face(self):
        self.assertGreater(np.ptp(face_samples(4)[:, 1]), 0.1)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SunTests(unittest.TestCase):
    def radiance_at_a_shadow_edge(self, sun_centre):
        floor = trimesh.Trimesh([(0, 0, 0), (0, 0, 8), (16, 0, 8), (16, 0, 0)], [(0, 1, 2), (0, 2, 3)], process=False)
        wall = trimesh.creation.box(extents=(1, 6, 8))
        wall.apply_translation((8.5, 3, 4))
        # The wall's top edge shades the floor from x = 3.2 on the sun's middle ray: this point is just lit.
        point, up = np.array([[3.0, 0.0, 4.0]]), np.array([[0.0, 1.0, 0.0]])
        return light(point, up, np.array([False]), RayMeshIntersector(trimesh.util.concatenate([floor, wall])), sun_only(25.0),
                     0.01, None, 1, sun_centre)[0, 0]

    def test_the_sun_disc_softens_a_shadow_edge_the_middle_ray_lights_fully(self):
        centre, disc = self.radiance_at_a_shadow_edge(True), self.radiance_at_a_shadow_edge(False)
        self.assertAlmostEqual(centre, 1.0 / np.hypot(0.8, 1.0), places=9)
        self.assertLess(disc, centre)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SpecTests(unittest.TestCase):
    def test_an_area_is_a_multiple_of_the_median(self):
        samples, knobs = parse_spec("samples=auto:2:8:median*0.5,sky=64,place=centroid,sun=centre", None, 10.0)
        self.assertEqual(samples, ("auto", 2, 8, 5.0))
        self.assertEqual(knobs, {"sky_rays": 64, "placement": "centroid", "sun_centre": True})

    def test_a_fixed_count_and_a_missing_spec_keep_the_declared_options(self):
        self.assertEqual(parse_spec("samples=fixed:4", None, 1.0)[0], (4, 1, 4, None))
        self.assertEqual(parse_spec("", ("auto", 1, 16, 3.0), 1.0), (("auto", 1, 16, 3.0), {"sun_centre": False}))

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
            with mock.patch("r3d.mesh_import.fetch_zip", return_value=root), mock.patch("r3d.mesh_import.REPO", root):
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
            with mock.patch("r3d.mesh_import.fetch_zip", return_value=root), mock.patch("r3d.mesh_import.REPO", root):
                scene = mesh_import.load_scene(scene_path)
                job = scene.renderers[0]
                geometry = mesh_import.bake_geometry(job, scene)
                declared = write_variant(job, scene, geometry, "", root / "a")
                other = write_variant(job, scene, geometry, "samples=fixed:1,place=centroid",
                                      root / "b")
            self.assertNotEqual(declared.read_bytes(), other.read_bytes())


if __name__ == "__main__":
    unittest.main()
