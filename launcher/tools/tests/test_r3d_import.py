"""Checks import settings, scene files, the lighting interface and that the tools name no scene."""

import contextlib
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

from r3d.import_settings import LIGHT_FIELDS, SettingsError, load_import_settings, load_scene, variant_settings  # noqa: E402
from r3d.scene_table import table_files, write_scene_table  # noqa: E402

try:
    import numpy as np

    from r3d import mesh_import
    from r3d.light import LIGHTS, encode_srgb8, light, to_srgb8
    from r3d.lit_mesh import read_lit_mesh
except ImportError:
    np = None

ROOT = pathlib.Path(__file__).resolve().parents[3]

SOURCE = '[source]\nurl = "https://example.invalid/m.zip"\nsha256 = "abc"\npath = "m.obj"\ncache = "m"\ncredit = "c"\n'
OUTPUT = '[output]\ndirectory = "."\nname = "mesh"\n'
AMBIENT = '[ambient]\ncolor = [1.0, 1.0, 1.0]\nintensity = 0.1\n'
TONEMAP = 'tonemap_white = 0.3\n'
THIN_STEP = '[process.thin]\nmaterial = "m"\nkeep = 0.5\n'
VISIBILITY_STEP = "[process.visibility]\nrounds = 2\n"
PATH_VISIBILITY_STEP = '[process.visibility]\nsource = "camera_path"\nevery_ms = 100\nsize = [8, 6]\nmargin = 2\n'
CAMERA_PATH = 'path = { tracks = "fly", node = "camera" }\n'
LIGHT_STEP = "[process.light]\nray_offset = 0.5\ncolour_merge_step = 6\n"
SIMPLIFY_STEP = '[process.simplify]\ndense_edge = 1.0\nprops = []\nprops_share = 0.3\nseal_seams = true\n'
VARIANT = '[[variants]]\nname = "mesh"\n'
CUBE = ("v 0 0 0\nv 8 0 0\nv 8 8 0\nv 0 8 0\nv 0 0 8\nv 8 0 8\nv 8 8 8\nv 0 8 8\nusemtl m\n"
        "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n")


def write_import(directory, name="mesh.import.toml", source=SOURCE, output=OUTPUT, body=""):
    path = pathlib.Path(directory) / name
    path.write_text(source + output + body)
    return path


def write_scene(directory, objects, head="", name="scene.toml"):
    path = pathlib.Path(directory) / name
    path.write_text(head + objects)
    return path


def renderer(mesh="mesh.import.toml", extra="", transform="", name=None):
    name = name or mesh.split(".")[0]
    return f'[[objects]]\nname = "{name}"\n{transform}[objects.mesh_renderer]\nmesh = "{mesh}"\n{extra}'


def sun_object(rotation="[0.0, 0.0, 0.0]", extra=""):
    return (f'[[objects]]\nname = "sun"\nrotation = {rotation}\n[objects.light]\ntype = "directional"\n'
            f"color = [1.0, 1.0, 1.0]\nintensity = 1.0\ndisc_degrees = 1.0\nrays = 1\n{extra}")


def camera(extra="", region=True):
    box = "region = { min = [0.0, 0.0, 0.0], max = [1.0, 1.0, 1.0] }\n" if region else ""
    return f'[[objects]]\nname = "camera"\n[objects.camera]\nhalf_fov_short_tan = 0.6\nnear_z = 1.0\n{box}{extra}'


def tree_scenes():
    """Every scene file in the tree."""
    return sorted(path for path in (ROOT / "launcher").rglob("*.scene.toml") if "build" not in path.parts)


class SettingsTests(unittest.TestCase):
    def rejects(self, pattern, **changes):
        with tempfile.TemporaryDirectory() as directory:
            path = write_import(directory, **changes)
            with self.assertRaisesRegex(SettingsError, pattern):
                load_import_settings(path)

    def test_basic_settings_turn_no_step_on(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory))
        self.assertEqual([variant.name for variant in settings.variants], ["mesh"])
        self.assertEqual((settings.alpha_keep, settings.visibility, settings.light, settings.simplify), (None,) * 4)

    def test_a_present_step_table_turns_the_step_on(self):
        body = "[process.alpha_mask]\nkeep_alpha = 0.5\n[process.visibility]\nrounds = 4\n" + THIN_STEP + LIGHT_STEP
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory, body=body))
        self.assertEqual((settings.alpha_keep, settings.visibility.rounds, settings.thin.keep, settings.light.ray_offset),
                         (0.5, 4, 0.5, 0.5))
        self.assertTrue(settings.scene_dependent)

    def test_only_light_and_visibility_make_an_import_scene_dependent(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory, body=THIN_STEP + "[process.alpha_mask]\nkeep_alpha = 0.5\n"))
        self.assertFalse(settings.scene_dependent)
        for body in (VISIBILITY_STEP, LIGHT_STEP):
            with tempfile.TemporaryDirectory() as directory:
                self.assertTrue(load_import_settings(write_import(directory, body=body)).scene_dependent)

    def test_visibility_reads_its_source(self):
        with tempfile.TemporaryDirectory() as directory:
            region = load_import_settings(write_import(directory, body=VISIBILITY_STEP)).visibility
            path = load_import_settings(write_import(directory, body=PATH_VISIBILITY_STEP)).visibility
        self.assertEqual((region.source, region.rounds), ("camera_region", 2))
        self.assertEqual((path.source, path.every_ms, path.size, path.samples, path.margin), ("camera_path", 100, (8, 6), 3, 2))
        self.rejects("process.visibility.source", body='[process.visibility]\nsource = "navmesh"\nrounds = 2\n')
        self.rejects("process.visibility", body='[process.visibility]\nsource = "camera_path"\nrounds = 2\n')
        self.rejects("margin cannot be negative", body=PATH_VISIBILITY_STEP.replace("margin = 2", "margin = -1"))

    def test_a_fit_recipe_reads_and_needs_a_lit_smooth_variant_with_room_to_prune(self):
        fit = ('fit = { budget = 8, train_every_ms = 1000, held_out_every_ms = 5000, coverage_every_ms = 100, steps = 20, '
               'batch = 4, laplacian = 10.0, normal_weight = 1.0, sha256 = "ab", recipe_sha256 = "cd" }\n')
        body = LIGHT_STEP + SIMPLIFY_STEP + VARIANT + "triangles = 10\n" + fit
        with tempfile.TemporaryDirectory() as directory:
            variant = load_import_settings(write_import(directory, body=body, output='[output]\ndirectory = "."\n')).variants[0]
        self.assertEqual((variant.fit.budget, variant.fit.normal_weight, variant.fit.sha256), (8, 1.0, "ab"))
        self.rejects("cannot exceed", body=body.replace("budget = 8", "budget = 11"), output='[output]\ndirectory = "."\n')
        self.rejects("smooth variant of a lit import", body=body.replace(LIGHT_STEP, ""), output='[output]\ndirectory = "."\n')
        self.rejects("fit", body=body.replace("steps = 20, ", ""), output='[output]\ndirectory = "."\n')

    def test_a_variant_visibility_table_overrides_the_imports(self):
        body = VISIBILITY_STEP + SIMPLIFY_STEP + VARIANT + "triangles = 10\n" + VARIANT.replace('"mesh"', '"pathed"')
        body += 'triangles = 10\nvisibility = { source = "camera_path", every_ms = 50, size = [8, 6] }\n'
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory, body=body, output='[output]\ndirectory = "."\n'))
        plain, pathed = settings.variants
        self.assertIsNone(plain.visibility)
        self.assertEqual((pathed.visibility.source, pathed.visibility.every_ms, pathed.visibility.margin), ("camera_path", 50, 0))
        self.assertEqual(settings.visibility.source, "camera_region")
        self.rejects("variants\\[1\\].visibility", body=body.replace("every_ms = 50, ", ""), output='[output]\ndirectory = "."\n')

    def test_the_bake_culls_with_the_variants_visibility_when_it_has_one(self):
        if np is None:
            self.skipTest("the r3d environment is not installed")
        own = SimpleNamespace(source="camera_path")
        shared = SimpleNamespace(source="camera_region")
        settings = SimpleNamespace(seed=1, position_scale=None, alpha_keep=None, visibility=shared, light=None, thin=None,
                                   simplify=None, double_sided=set())
        source = SimpleNamespace(p=np.zeros((3, 3)), uv=None, tri_v=np.array([[0, 1, 2]]), tri_t=None, tri_m=np.array([0]),
                                 names=["m"], textures=[None], materials={})
        seen = []
        with mock.patch.object(mesh_import, "load_source", return_value=source), \
                mock.patch.object(mesh_import, "RayMeshIntersector"), mock.patch.object(mesh_import.trimesh, "Trimesh"), \
                mock.patch.object(mesh_import, "visible_triangles", side_effect=lambda s, v, *rest: seen.append(v) or np.array([True])), \
                mock.patch.object(mesh_import, "shade_unlit", return_value=(np.zeros((3, 3)), np.zeros((3, 3)), np.array([[0, 1, 2]]))):
            mesh_import.bake_geometry(settings, SimpleNamespace(visibility=own, triangles=None), None)
            mesh_import.bake_geometry(settings, SimpleNamespace(visibility=None, triangles=None), None)
        self.assertEqual(seen, [own, shared])

    def test_check_fitted_names_what_changed_and_what_to_do(self):
        if np is None:
            self.skipTest("the r3d environment is not installed")
        with tempfile.TemporaryDirectory() as directory:
            settings = SimpleNamespace(mesh_dir=pathlib.Path(directory))
            (settings.mesh_dir / "m.mesh").write_bytes(b"mesh")
            fit = SimpleNamespace(sha256="0", recipe_sha256="r")
            variant = SimpleNamespace(name="m", fit=fit)
            with mock.patch("r3d.fitted_variant.recipe_digest", return_value="other"):
                with self.assertRaisesRegex(SystemExit, "recipe changed since the fit; rerun fitted_variant.py prepare\\|fit"):
                    mesh_import.check_fitted(settings, variant, None)
            with mock.patch("r3d.fitted_variant.recipe_digest", return_value="r"):
                with self.assertRaisesRegex(SystemExit, "not the 0 the fit recorded; rerun fitted_variant.py"):
                    mesh_import.check_fitted(settings, variant, None)

    def test_the_recipe_digest_follows_the_import_the_variant_and_the_tracks(self):
        if np is None:
            self.skipTest("the r3d environment is not installed")
        from r3d.fitted_variant import recipe_digest

        fit = ('fit = { budget = 8, train_every_ms = 1000, held_out_every_ms = 5000, coverage_every_ms = 100, steps = 20, '
               'batch = 4, laplacian = 10.0, normal_weight = 1.0, sha256 = "ab", recipe_sha256 = "cd" }\n')
        body = LIGHT_STEP + SIMPLIFY_STEP + VARIANT + "triangles = 10\n" + fit
        scene_ = SimpleNamespace(camera=SimpleNamespace(component=SimpleNamespace(path=SimpleNamespace(tracks="fly"))))
        with tempfile.TemporaryDirectory() as directory:
            (pathlib.Path(directory) / "fly_tracks_generated.c").write_text("tracks")
            output = '[output]\ndirectory = "."\n'

            def digest(body_, tracks="tracks"):
                (pathlib.Path(directory) / "fly_tracks_generated.c").write_text(tracks)
                settings = load_import_settings(write_import(directory, body=body_, output=output))
                return recipe_digest(settings, settings.variants[0], scene_)

            first = digest(body)
            self.assertEqual(digest(body.replace('sha256 = "ab"', 'sha256 = "ef"')), first, "the recorded hashes are not the recipe")
            self.assertNotEqual(digest(body.replace("steps = 20", "steps = 21")), first)
            self.assertNotEqual(digest(body.replace("ray_offset = 0.5", "ray_offset = 0.6")), first)
            self.assertNotEqual(digest(body, tracks="other tracks"), first)

    def test_each_committed_fitted_mesh_is_the_one_its_recipe_records(self):
        if np is None:
            self.skipTest("the r3d environment is not installed")
        import hashlib

        from r3d.fitted_variant import recipe_digest

        found = 0
        for path in tree_scenes():
            scene = load_scene(path)
            for item in scene.renderers:
                if item.variant.fit:
                    found += 1
                    mesh = item.settings.mesh_dir / f"{item.variant.name}.mesh"
                    self.assertEqual(hashlib.sha256(mesh.read_bytes()).hexdigest(), item.variant.fit.sha256, mesh.name)
                    self.assertEqual(recipe_digest(item.settings, item.variant, scene), item.variant.fit.recipe_sha256, mesh.name)
        self.assertGreater(found, 0, "no fitted variant: the check checks nothing")

    def test_a_seed_needs_a_step_that_draws_random_rays(self):
        self.rejects("process.seed", body="[process]\nseed = 3\n")
        self.rejects("process.seed", body="[process]\nseed = 3\n[process.alpha_mask]\nkeep_alpha = 0.5\n")
        with tempfile.TemporaryDirectory() as directory:
            load_import_settings(write_import(directory, body="[process]\nseed = 3\n" + VISIBILITY_STEP))

    def test_indirect_light_needs_complete_nonnegative_settings(self):
        self.rejects("process.light.indirect.bounces", body=LIGHT_STEP + "indirect = { bounces = -1, rays = 8, cache_samples = 1 }\n")
        self.rejects("process.light.indirect.rays", body=LIGHT_STEP + "indirect = { bounces = 1, rays = 0, cache_samples = 1 }\n")
        self.rejects("process.light.indirect.cache_samples", body=LIGHT_STEP + "indirect = { bounces = 1, rays = 8, cache_samples = 0 }\n")
        self.rejects("process.light.indirect.cache_samples is required", body=LIGHT_STEP + "indirect = { bounces = 1, rays = 8 }\n")
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(
                directory, body=LIGHT_STEP + "indirect = { bounces = 2, rays = 8, cache_samples = 1 }\n"))
        self.assertEqual((settings.light.indirect.bounces, settings.light.indirect.rays, settings.light.indirect.cache_samples),
                         (2, 8, 1))

    def test_a_variant_can_leave_the_indirect_light_out(self):
        bounced = LIGHT_STEP + "indirect = { bounces = 2, rays = 8, cache_samples = 1 }\n"
        output = '[output]\ndirectory = "."\n'
        self.rejects("indirect can only be false", body=bounced + VARIANT + "indirect = true\n", output=output)
        self.rejects("needs process.light.indirect", body=LIGHT_STEP + VARIANT + "indirect = false\n", output=output)
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(
                directory, body=bounced + VARIANT + "indirect = false\n" + VARIANT.replace("mesh", "lit"), output=output))
        dark, lit = settings.variants
        self.assertIsNone(variant_settings(settings, dark).light.indirect)
        self.assertEqual(variant_settings(settings, lit).light.indirect.bounces, 2)
        self.assertEqual(settings.light.indirect.bounces, 2, "the import's own settings are left alone")

    def test_variants_that_would_produce_the_same_mesh_are_rejected(self):
        variants = VARIANT + "triangles = 10\n" + VARIANT.replace("mesh", "other") + "triangles = 10\n"
        self.rejects("same mesh", body=SIMPLIFY_STEP + variants, output='[output]\ndirectory = "."\n')

    def test_the_output_directory_is_type_checked(self):
        self.rejects("output.directory", output='[output]\ndirectory = 3\nname = "mesh"\n')

    def test_the_committed_settings_and_scenes_load(self):
        scenes = tree_scenes()
        self.assertTrue(scenes)
        for path in scenes:
            self.assertTrue(load_scene(path).renderers, path.name)

    def test_each_committed_scene_table_is_what_its_scene_file_generates(self):
        for path in tree_scenes():
            for table, text in table_files(load_scene(path)):
                self.assertEqual(table.read_text().replace("\r\n", "\n"), text, f"{table.name} is stale: run scene_table.py")

    def test_no_two_scenes_bake_the_same_scene_dependent_mesh(self):
        owner = {}
        for path in tree_scenes():
            for item in load_scene(path).renderers:
                if item.settings.scene_dependent:
                    self.assertNotIn(item.variant.name, owner, f"{path.name} and {owner.get(item.variant.name)}")
                    owner[item.variant.name] = path.name

    def test_an_unknown_process_key_is_rejected(self):
        self.rejects("process.typo", body="[process.typo]\nx = 1\n")
        self.rejects("process.light.typo", body=LIGHT_STEP + "typo = 1\n")

    def test_auto_samples_with_a_minimum_over_its_maximum_are_rejected(self):
        variants = VARIANT + 'face_samples = { auto = { min = 3, max = 2, area = "median" } }\n'
        self.rejects("min", output='[output]\ndirectory = "."\n', body=LIGHT_STEP + "flat_sky_rays = 8\n" + variants)

    def test_a_source_without_a_sha_is_rejected(self):
        self.rejects("sha256", source=SOURCE.replace('sha256 = "abc"\n', ""))

    def test_an_unknown_key_is_rejected_in_a_table(self):
        self.rejects("typo", source=SOURCE + "typo = 1\n")

    def test_a_flag_and_a_count_are_type_checked(self):
        self.rejects("true or false", body=SIMPLIFY_STEP.replace("seal_seams = true", 'seal_seams = "no"') + VARIANT
                     + "triangles = 10\n", output='[output]\ndirectory = "."\n')
        self.rejects("must be an integer", body=SIMPLIFY_STEP + VARIANT + "triangles = 1.5\n", output='[output]\ndirectory = "."\n')

    def test_simplify_needs_a_budget_per_variant_and_a_budget_needs_simplify(self):
        self.rejects("needs variants", body=SIMPLIFY_STEP)
        self.rejects("triangles is required", body=SIMPLIFY_STEP + VARIANT, output='[output]\ndirectory = "."\n')
        self.rejects("needs process.simplify", body=VARIANT + "triangles = 10\n", output='[output]\ndirectory = "."\n')

    def test_face_samples_need_the_light_step_and_its_sky_rays(self):
        samples = "face_samples = { fixed = 4 }\n"
        self.rejects("needs process.light", body=VARIANT + samples, output='[output]\ndirectory = "."\n')
        self.rejects("flat_sky_rays is required", body=LIGHT_STEP + VARIANT + samples, output='[output]\ndirectory = "."\n')
        self.rejects("flat_sky_rays applies", body=LIGHT_STEP + "flat_sky_rays = 8\n" + VARIANT,
                     output='[output]\ndirectory = "."\n')

    def test_the_tools_name_no_scene(self):
        banned = set()
        for path in ROOT.glob("launcher/main/apps/**/meshes/*.import.toml"):
            values = tomllib.loads(path.read_text())
            banned.update(variant["name"].lower() for variant in values.get("variants", []))
            if "name" in values["output"]:
                banned.add(values["output"]["name"].lower())
        banned.update(path.name.removesuffix(".scene.toml").lower()
                      for path in ROOT.glob("launcher/main/apps/**/meshes/*.scene.toml"))
        self.assertTrue(banned)
        for path in sorted((ROOT / "launcher/tools/r3d").rglob("*")):
            if path.suffix in (".py", ".md", ".txt") and ".cache" not in path.parts:
                content = path.read_text().lower()
                for name in banned:
                    self.assertNotIn(name, content, path.name)


class SceneTests(unittest.TestCase):
    def rejects(self, pattern, setup, objects, head=""):
        with tempfile.TemporaryDirectory() as directory:
            setup(directory)
            path = write_scene(directory, objects, head)
            with self.assertRaisesRegex(SettingsError, pattern):
                load_scene(path)

    def two_imports(self, directory):
        write_import(directory, "a.import.toml", output='[output]\ndirectory = "."\nname = "a"\n')
        write_import(directory, "b.import.toml", output='[output]\ndirectory = "."\nname = "b"\n')

    def lit_import(self, directory):
        write_import(directory, body=LIGHT_STEP)

    def test_a_scene_with_two_mesh_renderers_validates(self):
        with tempfile.TemporaryDirectory() as directory:
            self.two_imports(directory)
            scene = load_scene(write_scene(directory, renderer("a.import.toml")
                                           + renderer("b.import.toml", transform="position = [1.0, 2.0, 3.0]\nscale = [2.0, 2.0, 2.0]\n")))
        self.assertEqual([item.variant.name for item in scene.renderers], ["a", "b"])
        self.assertEqual(scene.objects[0].position, [0.0, 0.0, 0.0])
        self.assertEqual(scene.objects[1].position, [1.0, 2.0, 3.0])
        self.assertEqual(scene.objects[1].scale, [2.0, 2.0, 2.0])

    def test_an_object_has_exactly_one_component(self):
        self.rejects("exactly one component", self.two_imports, '[[objects]]\nname = "empty"\n' + renderer("a.import.toml"))
        both = renderer("a.import.toml") + '[objects.camera]\nhalf_fov_short_tan = 0.6\nnear_z = 1.0\n'
        self.rejects("exactly one component", self.two_imports, both)

    def test_a_renderer_naming_a_missing_import_file_is_rejected(self):
        self.rejects("not a file", self.two_imports, renderer("a.import.toml") + renderer("gone.import.toml"))

    def test_a_scene_without_renderers_or_with_an_unknown_key_is_rejected(self):
        self.rejects("objects", self.two_imports, "", TONEMAP)
        self.rejects("mesh_renderer", self.two_imports, camera(region=False))
        self.rejects("typo", self.two_imports, renderer("a.import.toml"), "typo = 1\n")

    def test_object_names_and_baked_mesh_names_are_unique(self):
        self.rejects("names must be unique", self.two_imports, renderer("a.import.toml") + renderer("b.import.toml", name="a"))
        self.rejects("twice", self.two_imports, renderer("a.import.toml") + renderer("a.import.toml", name="again"))

    def test_a_variant_is_chosen_from_the_import_that_has_them(self):
        def setup(directory):
            write_import(directory, output='[output]\ndirectory = "."\n', body=VARIANT)

        self.rejects("variant is required", setup, renderer())
        self.rejects("not in", setup, renderer(extra='variant = "other"\n'))
        self.rejects("has no variants", self.two_imports, renderer("a.import.toml", 'variant = "a"\n'))

    def test_a_scene_dependent_step_needs_what_it_reads_from_the_scene(self):
        def culled(directory):
            write_import(directory, body=VISIBILITY_STEP)

        self.rejects("lights is required", self.lit_import, renderer())
        self.rejects("tonemap_white is required", self.lit_import, renderer(), AMBIENT)
        self.rejects("camera region is required", culled, renderer() + camera(region=False))

    def test_path_visibility_needs_the_camera_path_not_its_region(self):
        def culled(directory):
            write_import(directory, body=PATH_VISIBILITY_STEP)

        self.rejects("camera path is required", culled, renderer() + camera(region=False))
        self.rejects("camera region is read by no placed mesh", culled, renderer() + camera(CAMERA_PATH))
        with tempfile.TemporaryDirectory() as directory:
            culled(directory)
            scene = load_scene(write_scene(directory, renderer() + camera(CAMERA_PATH, region=False)))
        self.assertEqual(scene.camera.component.path.tracks, "fly")

    def test_scene_settings_no_placed_mesh_reads_are_rejected(self):
        self.rejects("lights is read by no placed mesh", self.two_imports, renderer("a.import.toml"), AMBIENT)
        self.rejects("lights is read by no placed mesh", self.two_imports, renderer("a.import.toml") + sun_object())
        self.rejects("tonemap_white is read by no placed mesh", self.two_imports, renderer("a.import.toml"), TONEMAP)
        self.rejects("camera region is read by no placed mesh", self.two_imports, renderer("a.import.toml") + camera())

    def test_the_indirect_look_defaults_to_physical_and_is_validated_strictly(self):
        def bounced(directory):
            write_import(directory, body=LIGHT_STEP + "indirect = { bounces = 1, rays = 8, cache_samples = 1 }\n")

        head = TONEMAP + AMBIENT
        tuned_table = "[indirect]\nintensity = 2.5\nalbedo_boost = 1.5\n"
        with tempfile.TemporaryDirectory() as directory:
            bounced(directory)
            plain = load_scene(write_scene(directory, renderer(), head))
            tuned = load_scene(write_scene(directory, renderer(), head + tuned_table))
        self.assertEqual((plain.indirect.intensity, plain.indirect.albedo_boost), (1.0, 1.0))
        self.assertEqual((tuned.indirect.intensity, tuned.indirect.albedo_boost), (2.5, 1.5))
        for pattern, table in (("scene.indirect.intensity must not be negative", "intensity = -1\n"),
                               ("scene.indirect.albedo_boost must be positive", "albedo_boost = 0\n"),
                               ("scene.indirect.intensity must be a number", 'intensity = "2"\n'),
                               ("scene.indirect.gain is not a known setting", "gain = 2\n")):
            self.rejects(pattern, bounced, renderer(), head + "[indirect]\n" + table)
        self.rejects("indirect settings is read by no placed mesh", self.lit_import, renderer(), head + tuned_table)

    def test_a_scene_dependent_mesh_is_baked_where_it_sits(self):
        placed = renderer(transform="position = [1.0, 0.0, 0.0]\n")
        self.rejects("identity", self.lit_import, placed, TONEMAP + AMBIENT)
        with tempfile.TemporaryDirectory() as directory:
            self.two_imports(directory)
            load_scene(write_scene(directory, renderer("a.import.toml", transform="position = [1.0, 0.0, 0.0]\n")))

    def test_a_directional_lights_direction_comes_from_its_rotation(self):
        with tempfile.TemporaryDirectory() as directory:
            self.lit_import(directory)
            overhead = load_scene(write_scene(directory, renderer() + sun_object(), TONEMAP + AMBIENT)).lights[0]["direction"]
            turned = load_scene(write_scene(directory, renderer() + sun_object("[90.0, 90.0, 0.0]"), TONEMAP + AMBIENT)).lights[0]["direction"]
        for got, want in ((overhead, [0.0, 1.0, 0.0]), (turned, [1.0, 0.0, 0.0])):
            self.assertTrue(all(abs(a - b) < 1e-12 for a, b in zip(got, want)), got)

    def test_the_lights_are_the_directional_objects_then_the_sky_then_the_ambient(self):
        sky = '[sky]\ncolor = [0.5, 0.5, 0.5]\nintensity = 1.0\nrays = 2\n'
        with tempfile.TemporaryDirectory() as directory:
            self.lit_import(directory)
            scene = load_scene(write_scene(directory, renderer() + sun_object(), TONEMAP + AMBIENT + sky))
        self.assertEqual([light["type"] for light in scene.lights], ["directional", "sky", "ambient"])

    def test_a_camera_carries_its_lens_and_region_and_a_scene_has_one(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory, body=VISIBILITY_STEP)
            scene = load_scene(write_scene(directory, renderer() + camera()))
        self.assertEqual(scene.camera.component.near_z, 1.0)
        self.assertEqual(scene.region[1], [1.0, 1.0, 1.0])
        self.rejects("one camera", self.two_imports, renderer("a.import.toml") + camera(region=False)
                     + camera(region=False).replace('"camera"', '"other"'))
        self.rejects("half_fov_short_tan", self.two_imports, renderer("a.import.toml")
                     + '[[objects]]\nname = "c"\n[objects.camera]\nnear_z = 1.0\n')
        self.rejects("C identifier", self.two_imports, renderer("a.import.toml")
                     + camera(region=False, extra='path = { tracks = "no-good", node = "camera" }\n'))

    def test_an_unknown_key_is_rejected_in_a_light_and_in_the_sky(self):
        self.rejects("typo", self.two_imports, renderer("a.import.toml") + sun_object(extra="typo = 1\n"))
        self.rejects("typo", self.lit_import, renderer(), TONEMAP + AMBIENT + "[sky]\ncolor = [1.0, 1.0, 1.0]\n"
                     "intensity = 1.0\nrays = 1\ntypo = 1\n")

    def test_a_reserved_light_type_is_rejected(self):
        point = '[[objects]]\nname = "p"\n[objects.light]\ntype = "point"\n'
        self.rejects("reserved", self.two_imports, renderer("a.import.toml") + point)

    def test_the_scene_table_is_one_definition_with_an_entity_per_renderer_and_the_camera(self):
        flight = 'path = { tracks = "flight", node = "rig" }\n'
        placed = renderer("b.import.toml", transform="position = [1.0, 2.0, 3.0]\nscale = [2.0, 2.0, 2.0]\n")
        with tempfile.TemporaryDirectory() as directory:
            self.two_imports(directory)
            path = write_scene(directory, renderer("a.import.toml") + placed + camera(region=False, extra=flight),
                               name="hall.scene.toml")
            written = write_scene_table(load_scene(path))
            source = written[0].read_text()
            header = written[1].read_text()
            self.assertEqual([item.name for item in written], ["hall_scene_generated.c", "hall_scene_generated.h"])
        self.assertIn('.name = "hall",', source)
        self.assertIn('static const scene_renderer_def_t hall_scene_renderers[] = {\n    {0, "a"},\n    {1, "b"},\n};', source)
        self.assertIn("{.m = {{2.0F, 0.0F, 0.0F}, {0.0F, 2.0F, 0.0F}, {0.0F, 0.0F, 2.0F}}, .position = {1.0F, 2.0F, 3.0F}},", source)
        self.assertIn(".clip = &flight_clip, .translation = &flight_rig_translation, .rotation = &flight_rig_rotation", source)
        self.assertIn("{2, {.half_fov_short_tan = 0.6F, .near_z = 1.0F, .placement = NULL, .path = &hall_scene_camera_path}, 0x000000},",
                      source)
        self.assertIn(".entity_count = 3,", source)
        self.assertIn("SCENE_REGISTER(hall_scene)", source)
        self.assertIn('#include "scene/scene.h"', source)
        self.assertNotIn("scene_shell", source)
        self.assertIn("extern const scene_def_t hall_scene;", header)
        self.assertIn("#define HALL_SCENE_B ((scene_entity_t)1)", header)
        self.assertIn("#define HALL_SCENE_CAMERA ((scene_entity_t)2)", header)

    def test_a_camera_background_reaches_its_table_entry_and_defaults_to_black(self):
        for extra, wanted in (("background = 0x336699" + chr(10), "0x336699"), ("", "0x000000")):
            with tempfile.TemporaryDirectory() as directory:
                self.two_imports(directory)
                path = write_scene(directory, renderer("a.import.toml") + camera(region=False, extra=extra), name="hall.scene.toml")
                source = write_scene_table(load_scene(path))[0].read_text()
            self.assertIn(".placement = NULL, .path = NULL}, " + wanted + "},", source)

    def table_of(self, objects, lit=False):
        """The generated source and header of a scene, as text; `lit` lights both of its meshes."""
        body = LIGHT_STEP if lit else ""
        with tempfile.TemporaryDirectory() as directory:
            for mesh in ("a", "b"):
                output = f'[output]\ndirectory = "."\nname = "{mesh}"\n'
                write_import(directory, f"{mesh}.import.toml", output=output, body=body)
            head = TONEMAP + AMBIENT if lit else ""
            scene = load_scene(write_scene(directory, objects, head, name="hall.scene.toml"))
            source, header = (text for _, text in table_files(scene))
        return source, header

    def test_names_transforms_renderers_and_macros_share_one_order(self):
        # The first entity is the only one that moved, so an array reversed or shifted is caught.
        moved = renderer("a.import.toml", transform="position = [1.0, 2.0, 3.0]\n")
        source, header = self.table_of(moved + renderer("b.import.toml") + camera(region=False))
        names = re.search(r"hall_scene_names\[\] = \{(.*?)\};", source).group(1).replace('"', "").split(", ")
        transforms = re.search(r"hall_scene_transforms\[\] = \{\n(.*?)\n\};", source, re.S).group(1).splitlines()
        indices = [int(i) for i in re.findall(r"^    \{(\d+), \"", source, re.M)]
        macros = re.findall(r"#define HALL_SCENE_(\w+) \(\(scene_entity_t\)(\d+)\)", header)
        self.assertEqual(names, ["a", "b", "camera"])
        self.assertEqual([name.lower() for name, _ in macros], names)
        self.assertEqual([int(index) for _, index in macros], [0, 1, 2])
        self.assertEqual(len(transforms), len(names))
        self.assertEqual([("position = {1.0F, 2.0F, 3.0F}" in line) for line in transforms], [True, False, False])
        self.assertEqual(indices[:2], [names.index("a"), names.index("b")])

    def test_a_light_has_no_entity_and_the_indices_step_over_it(self):
        sun = sun_object()
        source, header = self.table_of(renderer("a.import.toml") + sun + renderer("b.import.toml") + camera(region=False),
                                       lit=True)
        self.assertIn('hall_scene_names[] = {"a", "b", "camera"};', source)
        self.assertIn("{0, \"a\"},", source)
        self.assertIn("{1, \"b\"},", source)
        self.assertIn("{2, {.half_fov_short_tan", source)
        self.assertNotIn("SUN", header)
        self.assertIn(".entity_count = 3,", source)

    def test_the_scene_table_holds_only_what_the_device_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory, body=LIGHT_STEP + VISIBILITY_STEP)
            scene = load_scene(write_scene(directory, renderer() + sun_object() + camera(), TONEMAP + AMBIENT,
                                           name="hall.scene.toml"))
            source = "".join(text for _, text in table_files(scene))
        self.assertNotIn("sun", source)
        self.assertNotIn("region", source)
        self.assertNotIn("1.0F, 1.0F, 1.0F}}", source.replace("{{{1.0F", "x"))

    def test_a_placement_is_baked_as_rotation_times_scale(self):
        quarter = "rotation = [0.0, 90.0, 0.0]\nscale = [1.0, 2.0, 3.0]\n"
        with tempfile.TemporaryDirectory() as directory:
            self.two_imports(directory)
            scene = load_scene(write_scene(directory, renderer("a.import.toml", transform=quarter)))
        matrix = scene.objects[0].matrix
        want = ((0.0, 0.0, 3.0), (0.0, 2.0, 0.0), (-1.0, 0.0, 0.0))
        for row, expected in zip(matrix, want):
            self.assertTrue(all(abs(a - b) < 1e-12 for a, b in zip(row, expected)), matrix)

    def test_a_scale_must_be_positive_and_a_symbol_name_an_identifier(self):
        self.rejects("positive", self.two_imports, renderer("a.import.toml", transform="scale = [1.0, -1.0, 1.0]\n"))
        self.rejects("positive", self.two_imports, renderer("a.import.toml", transform="scale = [0.0, 1.0, 1.0]\n"))
        self.rejects("C identifier", self.two_imports, renderer("a.import.toml", name="not a symbol"))

    def test_a_scene_table_needs_its_meshes_in_one_output_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory, "a.import.toml", output='[output]\ndirectory = "."\nname = "a"\n')
            pathlib.Path(directory, "other").mkdir()
            write_import(directory, "b.import.toml", output='[output]\ndirectory = "other"\nname = "b"\n')
            scene = load_scene(write_scene(directory, renderer("a.import.toml") + renderer("b.import.toml")))
            with self.assertRaisesRegex(ValueError, "one output directory"):
                table_files(scene)

    def test_sky_and_ambient_are_not_light_objects(self):
        sky = '[[objects]]\nname = "s"\n[objects.light]\ntype = "sky"\n'
        self.rejects("scene settings", self.two_imports, renderer("a.import.toml") + sky)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class AuthoredImportTests(unittest.TestCase):
    def bake(self, directory, body="", scene=False, output=OUTPUT):
        (pathlib.Path(directory) / "m.obj").write_text(CUBE)
        (pathlib.Path(directory) / "m.mtl").write_text("newmtl m\nKd 0.5 0.25 0.125\n")
        path = write_import(directory, output=output, body=body)
        if scene:
            path = write_scene(directory, renderer(), name="mesh.scene.toml")
        with mock.patch("r3d.mesh_import.fetch_zip", return_value=pathlib.Path(directory)), \
                mock.patch("r3d.mesh_import.REPO", pathlib.Path(directory)):
            return mesh_import.main([str(path)])

    def test_basic_settings_keep_the_source_triangles_and_run_no_step(self):
        steps = ("drop_masked", "visible_from_region", "light", "simplify", "densify", "face_colours",
                 "merge_matching_colours")
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
            for name in steps:
                stack.enter_context(mock.patch(f"r3d.mesh_import.{name}", side_effect=AssertionError(name)))
            self.assertEqual(self.bake(directory), 0)
            mesh = read_lit_mesh(pathlib.Path(directory) / "mesh.mesh")
        self.assertEqual(len(mesh.tris), 12)
        self.assertEqual(len(mesh.pos), 8)

    def test_a_scene_run_bakes_the_mesh_it_places(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.bake(directory, scene=True), 0)
            self.assertTrue((pathlib.Path(directory) / "mesh.mesh").is_file())

    def test_an_import_writes_its_mesh_beside_the_import_file_not_in_the_output_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            pathlib.Path(directory, "generated").mkdir()
            self.assertEqual(self.bake(directory, output='[output]\ndirectory = "generated"\nname = "mesh"\n'), 0)
            beside = (pathlib.Path(directory) / "mesh.mesh").is_file()
            elsewhere = list(pathlib.Path(directory, "generated").iterdir())
        self.assertTrue(beside)
        self.assertEqual(elsewhere, [])

    def test_a_scene_dependent_import_refuses_to_bake_alone(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()) as error:
            with self.assertRaises(SystemExit):
                self.bake(directory, body=LIGHT_STEP)
        self.assertIn("needs a scene", error.getvalue())

    def test_unlit_colour_is_the_material_albedo(self):
        with tempfile.TemporaryDirectory() as directory:
            self.bake(directory)
            mesh = read_lit_mesh(pathlib.Path(directory) / "mesh.mesh")
        target = 255.0 * np.array([0.5, 0.25, 0.125])
        self.assertTrue((np.abs(mesh.rgb - target) <= 8).all(), mesh.rgb[:2])


class ClearIntersector:
    def intersects_any(self, origins, directions):
        return np.zeros(len(origins), dtype=bool)


def sun(direction, color, intensity):
    return {"type": "directional", "direction": direction, "color": color, "intensity": intensity,
            "disc_degrees": 0, "rays": 1}


def radiance(lights, normal, double_sided):
    return light(np.array([[0.0, 0.0, 0.0]]), np.array([normal]), np.array([double_sided]), ClearIntersector(),
                 lights, 0.5, np.random.default_rng(1))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class LightListTests(unittest.TestCase):
    def test_directional_lights_add_their_radiance(self):
        got = radiance([sun([0, 1, 0], [1, 0, 0], 2), sun([0, 1, 0], [0, 1, 0], 3)], [0.0, 1.0, 0.0], False)
        np.testing.assert_allclose(got, [[2.0, 3.0, 0.0]])

    def test_the_order_of_the_lights_does_not_change_the_radiance(self):
        lights = [sun([0, 1, 0], [1, 0, 0], 2), sun([1, 0.1, 0], [0, 1, 0], 1),
                  {"type": "sky", "color": [0, 0, 1], "intensity": 1, "rays": 1},
                  {"type": "ambient", "color": [1, 1, 1], "intensity": 0.1}]
        for normal in ([0.0, -1.0, 0.0], [-1.0, 0.0, 0.0], [0.0, 1.0, 0.0]):
            for double_sided in (False, True):
                forward = radiance(lights, normal, double_sided)
                np.testing.assert_allclose(radiance(lights[::-1], normal, double_sided), forward)

    def test_a_double_sided_face_turns_to_the_light(self):
        got = radiance([sun([0, 1, 0], [1, 1, 1], 1)], [0.0, -1.0, 0.0], True)
        np.testing.assert_allclose(got, [[1.0, 1.0, 1.0]])

    def test_the_unlit_encoder_is_the_tone_mapped_one_without_a_tone_map(self):
        linear = np.array([0.0, 0.1, 0.5, 1.0, 2.0])
        np.testing.assert_array_equal(encode_srgb8(linear), to_srgb8(linear, 0.0))
        self.assertEqual(encode_srgb8(np.array([0.5]))[0], round(255 * 0.5 ** (1 / 2.2)))

    def test_every_light_type_has_a_baker(self):
        self.assertEqual(set(LIGHTS), set(LIGHT_FIELDS))


if __name__ == "__main__":
    unittest.main()
