"""Checks geometry imports and the scene renderers that bake them."""

import hashlib
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.import_settings import SettingsError, load_import_settings, load_scene
from r3d.scene_table import table_files

try:
    import numpy as np
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
        self.assertIsNone(settings.light)

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
        self.assertEqual((job.bake.ray_offset, job.visibility.source, job.asset_name), (0.5, "camera_path", "scene.mesh"))

    def test_a_renderer_without_bake_is_albedo_only(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            scene = load_scene(write_scene(directory, renderer()))
        self.assertIsNone(scene.renderers[0].bake)
        self.assertIsNone(scene.renderers[0].settings.light)

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


@unittest.skipIf(np is None, "the r3d environment is not installed")
class FittedStampTests(unittest.TestCase):
    def test_parsed_effective_recipe_survives_the_move(self):
        from r3d.fitted_variant import recipe_digest

        scene = load_scene(ROOT / "launcher/main/apps/render_lab/meshes/sponza.scene.toml")
        job = next(item for item in scene.renderers if item.variant.fit)
        before_settings = SimpleNamespace(**{**vars(job.settings), "visibility": job.variant.visibility})
        before_variant = SimpleNamespace(**{**vars(job.variant), "visibility": None})
        self.assertEqual(recipe_digest(before_settings, before_variant, scene), recipe_digest(job.settings, job.variant, scene))

    def test_each_fitted_scene_output_matches_its_stamp(self):
        from r3d.fitted_variant import recipe_digest

        found = 0
        for path in tree_scenes():
            scene = load_scene(path)
            for job in scene.renderers:
                if job.variant.fit:
                    found += 1
                    self.assertEqual(hashlib.sha256(job.asset_path.read_bytes()).hexdigest(), job.variant.fit.sha256, job.asset_path.name)
                    self.assertEqual(recipe_digest(job.settings, job.variant, scene), job.variant.fit.recipe_sha256, job.asset_path.name)
        self.assertGreater(found, 0)


if __name__ == "__main__":
    unittest.main()
