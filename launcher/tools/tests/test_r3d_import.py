"""Checks geometry imports and the scene renderers that bake them."""

import contextlib
import hashlib
import io
import pathlib
import re
import sys
import tempfile
import tomllib
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.import_settings import LIGHT_FIELDS, SettingsError, load_import_settings, load_scene
from r3d.scene_table import table_files, write_scene_table

try:
    import numpy as np

    from r3d import mesh_import
    from r3d.light import LIGHTS, encode_srgb8, light, to_srgb8
    from r3d.lit_mesh import read_lit_mesh
except ImportError:
    np = None

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ('[source]\nurl = "https://example.invalid/m.zip"\nsha256 = "abc"\npath = "m.obj"\n'
          'cache = "m"\ncredit = "c"\n')
OUTPUT = '[output]\ndirectory = "."\nname = "mesh"\n'
AMBIENT = '[ambient]\ncolor = [1.0, 1.0, 1.0]\nintensity = 0.1\n'
TONEMAP = 'tonemap_white = 0.3\n'
SIMPLIFY = '[geometry]\nsimplify = { dense_edge = 1.0, props = [], props_share = 0.3, seal_seams = true }\n'
VARIANT = '[[variants]]\nname = "mesh"\ntriangles = 10\n'
BAKE = '[bake]\nray_offset = 0.5\ncolour_merge_step = 6\n'
CUBE = ("v 0 0 0\nv 8 0 0\nv 8 8 0\nv 0 8 0\nv 0 0 8\nv 8 0 8\nv 8 8 8\nv 0 8 8\nusemtl m\n"
        "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n")


def write_import(directory, name="mesh.import.toml", source=SOURCE, output=OUTPUT, body=""):
    path = pathlib.Path(directory) / name
    path.write_text(source + output + body)
    return path


def write_scene(directory, objects, head="", name="scene.scene.toml"):
    path = pathlib.Path(directory) / name
    path.write_text(head + objects)
    return path


def renderer(mesh="mesh.import.toml", extra="", transform="", name=None):
    name = name or mesh.split(".")[0]
    return f'[[objects]]\nname = "{name}"\n{transform}[objects.mesh_renderer]\nmesh = "{mesh}"\n{extra}'


def sun_object(rotation="[0.0, 0.0, 0.0]"):
    return (f'[[objects]]\nname = "sun"\nrotation = {rotation}\n[objects.light]\ntype = "directional"\n'
            'color = [1.0, 1.0, 1.0]\nintensity = 1.0\ndisc_degrees = 1.0\nrays = 1\n')


def camera(path=False, region=True):
    track = 'path = { tracks = "fly", node = "camera" }\n' if path else ''
    bounds = 'region = { min = [0.0, 0.0, 0.0], max = [1.0, 1.0, 1.0] }\n' if region else ''
    return ('[[objects]]\nname = "camera"\n[objects.camera]\nhalf_fov_short_tan = 0.6\nnear_z = 1.0\n'
            + bounds + track)


def tree_scenes():
    return sorted(path for path in (ROOT / "launcher").rglob("*.scene.toml") if not {"build", "results"} & set(path.parts))


class ImportTests(unittest.TestCase):
    def rejects(self, pattern, body):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(SettingsError, pattern):
                output = '[output]\ndirectory = "."\n' if "[[variants]]" in body else OUTPUT
                load_import_settings(write_import(directory, output=output, body=body))

    def test_an_import_contains_geometry_only(self):
        body = ('[process]\nseed = 3\n[geometry]\nalpha_mask = { keep_alpha = 0.5 }\n'
                'thin = { material = "m", keep = 0.5 }\n' + SIMPLIFY.removeprefix("[geometry]\n") + VARIANT)
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory, output='[output]\ndirectory = "."\n', body=body))
        self.assertEqual((settings.seed, settings.alpha_keep, settings.thin.keep, settings.variants[0].triangles), (3, 0.5, 0.5, 10))
        self.assertFalse(hasattr(settings, "light"))

    def test_old_scene_owned_keys_name_each_new_home(self):
        cases = (
            ('[visibility]\nrounds = 2\n', r"\[visibility\] moved to objects\.mesh_renderer\.visibility"),
            ('[lighting]\nlight = { ray_offset = 0.5, colour_merge_step = 6 }\n', r"\[lighting\] moved to scene \[bake\]"),
            (VARIANT + 'shading = "smooth"\n', r"variants\[0\]\.shading moved to objects\.mesh_renderer\.shading"),
            (VARIANT + 'visibility = { rounds = 2 }\n', r"variants\[0\]\.visibility moved to objects\.mesh_renderer\.visibility"),
            (VARIANT + 'indirect = false\n', r"variants\[0\]\.indirect moved to objects\.mesh_renderer\.indirect"),
        )
        for body, message in cases:
            with self.subTest(body=body):
                self.rejects(message, body)

    def test_a_seed_requires_the_geometry_step_that_draws_it(self):
        self.rejects("process.seed needs geometry.thin", '[process]\nseed = 3\n')

    def test_variants_hold_only_their_name_and_triangle_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory, output='[output]\ndirectory = "."\n', body=SIMPLIFY + VARIANT))
        self.assertEqual(vars(settings.variants[0]), {"name": "mesh", "triangles": 10})


class SceneTests(unittest.TestCase):
    def test_a_scene_renderer_owns_its_bake_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory, output='[output]\ndirectory = "."\n', body=SIMPLIFY + VARIANT)
            scene = load_scene(write_scene(
                directory, renderer(extra='variant = "mesh"\nbake = true\nshading = "smooth"\n'
                                           'visibility = { source = "camera_path", every_ms = 100, size = [8, 6] }\n')
                + sun_object() + camera(path=True, region=False), TONEMAP + AMBIENT + BAKE))
        job = scene.renderers[0]
        self.assertEqual((scene.bake.ray_offset, job.renderer.visibility.source, job.asset_name),
                         (0.5, "camera_path", "scene.mesh"))

    def test_a_renderer_without_bake_is_albedo_only(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            scene = load_scene(write_scene(directory, renderer()))
        self.assertFalse(scene.renderers[0].renderer.bake)
        self.assertFalse(hasattr(scene.renderers[0].settings, "light"))

    def test_two_scenes_baking_one_import_have_separate_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            head = TONEMAP + AMBIENT + BAKE
            objects = renderer(extra='bake = true\n') + sun_object()
            first = load_scene(write_scene(directory, objects, head, "first.scene.toml"))
            second = load_scene(write_scene(directory, objects, head, "second.scene.toml"))
        self.assertEqual((first.renderers[0].asset_name, second.renderers[0].asset_name), ("first.mesh", "second.mesh"))
        self.assertNotEqual(first.renderers[0].asset_path, second.renderers[0].asset_path)

    def test_a_baked_renderer_requires_bake_settings_and_lights(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            objects = renderer(extra='bake = true\n')
            with self.assertRaisesRegex(SettingsError, r"scene \[bake\] is required"):
                load_scene(write_scene(directory, objects))
            with self.assertRaisesRegex(SettingsError, "scene lights is required"):
                load_scene(write_scene(directory, objects, TONEMAP + BAKE))

    def test_flat_and_indirect_options_need_their_scene_bake_knobs(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            objects = renderer(extra='bake = true\nshading = { flat = { fixed = 4 } }\n') + sun_object()
            with self.assertRaisesRegex(SettingsError, "flat_sky_rays is required"):
                load_scene(write_scene(directory, objects, TONEMAP + BAKE))
            objects = renderer(extra='bake = true\nindirect = false\n') + sun_object()
            with self.assertRaisesRegex(SettingsError, "needs scene.bake.indirect"):
                load_scene(write_scene(directory, objects, TONEMAP + BAKE))

    def test_committed_scene_tables_match_their_inputs(self):
        for path in tree_scenes():
            for table, text in table_files(load_scene(path)):
                self.assertEqual(table.read_text().replace("\r\n", "\n"), text, table.name)

    def test_a_scene_table_belongs_beside_its_scene(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory, output='[output]\ndirectory = "generated"\nname = "mesh"\n')
            scene = load_scene(write_scene(directory, renderer(), name="hall.scene.toml"))
            tables = table_files(scene)
        self.assertEqual([item.parent for item, _ in tables], [scene.path.parent, scene.path.parent])

    def test_baked_output_names_the_scene_and_object_but_plain_output_names_the_import(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            plain = load_scene(write_scene(directory, renderer(name="copy")))
            baked = load_scene(write_scene(directory, renderer(extra="bake = true\n", name="copy") + sun_object(),
                                            TONEMAP + AMBIENT + BAKE, "hall.scene.toml"))
        self.assertEqual(plain.renderers[0].asset_name, "mesh")
        self.assertEqual(baked.renderers[0].asset_name, "hall.copy")
        self.assertEqual(plain.renderers[0].asset_path.name, "mesh.mesh")
        self.assertEqual(baked.renderers[0].asset_path.name, "hall.copy.mesh")

    def test_asset_names_are_limited_by_the_pack(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            name = "a" * 28
            with self.assertRaisesRegex(SettingsError, "31-byte limit"):
                load_scene(write_scene(directory, renderer(name=name, extra="bake = true\n") + sun_object(),
                                       TONEMAP + AMBIENT + BAKE, "hall.scene.toml"))

    def test_unknown_keys_and_renderer_steps_are_validated_at_their_scene_homes(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            checks = (
                ("shading = { flat = { fixed = 2 }, typo = 1 }\n", "shading.typo"),
                ("visibility = { source = \"camera_region\", rounds = 1, typo = 1 }\n", "visibility.typo"),
                ("fit = { budget = 1 }\n", "fit"),
            )
            for extra, pattern in checks:
                with self.subTest(extra=extra), self.assertRaisesRegex(SettingsError, pattern):
                    load_scene(write_scene(directory, renderer(extra="bake = true\n" + extra) + sun_object(), TONEMAP + AMBIENT + BAKE))

    def test_indirect_look_is_strict_and_needs_a_baked_indirect_renderer(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            base = TONEMAP + AMBIENT + BAKE.replace("\n", "\n", 1).replace(
                "colour_merge_step = 6", "colour_merge_step = 6\nindirect = { bounces = 1, rays = 2, cache_samples = 1 }")
            objects = renderer(extra="bake = true\n") + sun_object()
            scene = load_scene(write_scene(directory, objects, base + "[indirect]\nintensity = 2.5\nalbedo_boost = 1.5\n"))
            self.assertEqual((scene.indirect.intensity, scene.indirect.albedo_boost), (2.5, 1.5))
            with self.assertRaisesRegex(SettingsError, "albedo_boost must be positive"):
                load_scene(write_scene(directory, objects, base + "[indirect]\nalbedo_boost = 0\n"))

    def test_lights_camera_and_transforms_are_authored_by_the_scene(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            scene = load_scene(write_scene(
                directory, renderer(extra="bake = true\n") + sun_object("[90.0, 90.0, 0.0]")
                + camera(path=True, region=False), TONEMAP + AMBIENT + BAKE))
        self.assertTrue(all(abs(a - b) < 1e-12 for a, b in zip(scene.lights[0]["direction"], [1.0, 0.0, 0.0])))
        self.assertEqual(scene.camera.component.path.tracks, "fly")

    def test_identical_import_variant_budgets_are_rejected(self):
        variants = VARIANT + VARIANT.replace('name = "mesh"', 'name = "copy"')
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(SettingsError, "same mesh"):
                load_import_settings(write_import(directory, output='[output]\ndirectory = "."\n', body=SIMPLIFY + variants))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class AuthoredImportTests(unittest.TestCase):
    def bake(self, directory, scene=False):
        root = pathlib.Path(directory)
        (root / "m.obj").write_text(CUBE)
        (root / "m.mtl").write_text("newmtl m\nKd 0.5 0.25 0.125\n")
        import_path = write_import(root)
        path = write_scene(root, renderer(), name="mesh.scene.toml") if scene else import_path
        with mock.patch("r3d.mesh_import.fetch_zip", return_value=root):
            return mesh_import.main([str(path)])

    def test_a_bare_import_runs_the_albedo_bake(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.bake(directory), 0)
            mesh = read_lit_mesh(pathlib.Path(directory) / "mesh.mesh")
        self.assertEqual(len(mesh.tris), 12)
        target = 255.0 * np.array([0.5, 0.25, 0.125])
        self.assertTrue((np.abs(mesh.rgb - target) <= 8).all(), mesh.rgb[:2])

    def test_an_unbaked_renderer_runs_the_same_albedo_bake(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.bake(directory, scene=True), 0)
            mesh = read_lit_mesh(pathlib.Path(directory) / "mesh.mesh")
        self.assertEqual(len(mesh.tris), 12)


class ClearIntersector:
    def intersects_any(self, origins, directions):
        return np.zeros(len(origins), dtype=bool)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class LightListTests(unittest.TestCase):
    def test_directional_lights_add_their_radiance_and_order_does_not_matter(self):
        lights = [
            {"type": "directional", "direction": [0, 1, 0], "color": [1, 0, 0], "intensity": 2,
             "disc_degrees": 0, "rays": 1},
            {"type": "directional", "direction": [0, 1, 0], "color": [0, 1, 0], "intensity": 3,
             "disc_degrees": 0, "rays": 1},
        ]
        args = (np.array([[0.0, 0.0, 0.0]]), np.array([[0.0, 1.0, 0.0]]), np.array([False]), ClearIntersector())
        first = light(*args, lights, 0.5, np.random.default_rng(1))
        np.testing.assert_allclose(first, [[2.0, 3.0, 0.0]])
        np.testing.assert_allclose(light(*args, lights[::-1], 0.5, np.random.default_rng(1)), first)

    def test_the_light_reader_and_bakers_agree(self):
        self.assertEqual(set(LIGHTS), set(LIGHT_FIELDS))


class DigestTests(unittest.TestCase):
    def test_the_recipe_digest_ignores_equivalent_toml_layouts(self):
        from r3d.fitted_variant import recipe_digest

        fit = ('fit = { budget = 8, train_every_ms = 1000, held_out_every_ms = 5000, coverage_every_ms = 100, steps = 20, '
               'batch = 4, laplacian = 10.0, normal_weight = 1.0, sha256 = "ab", recipe_sha256 = "cd" }\n')
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "fly_tracks_generated.c").write_text("tracks")
            write_import(root, output='[output]\ndirectory = "."\n', body=SIMPLIFY + VARIANT)
            objects = renderer(extra='variant = "mesh"\nbake = true\nvisibility = { source = "camera_path", every_ms = 10, size = [8, 6] }\n' + fit)
            first = load_scene(write_scene(root, objects + sun_object() + camera(path=True, region=False), TONEMAP + AMBIENT + BAKE))
            alternate = (TONEMAP + AMBIENT + '[bake]\ncolour_merge_step = 6\nray_offset = 0.5\n')
            second = load_scene(write_scene(root, objects + sun_object() + camera(path=True, region=False), alternate,
                                            "other.scene.toml"))
            first_digest = recipe_digest(first.renderers[0].settings, first.renderers[0].renderer, first)
            second_digest = recipe_digest(second.renderers[0].settings, second.renderers[0].renderer, second)
        self.assertEqual(first_digest, second_digest)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class FittedStampTests(unittest.TestCase):
    def test_each_fitted_scene_output_matches_its_stamp(self):
        from r3d.fitted_variant import recipe_digest

        found = 0
        for path in tree_scenes():
            scene = load_scene(path)
            for job in scene.renderers:
                if job.renderer.fit:
                    found += 1
                    self.assertEqual(recipe_digest(job.settings, job.renderer, scene), job.renderer.fit.recipe_sha256,
                                     job.asset_path.name)
        self.assertGreater(found, 0)


if __name__ == "__main__":
    unittest.main()
