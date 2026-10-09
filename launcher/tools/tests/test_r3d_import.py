"""Checks geometry imports and the scene renderers that bake them."""

import contextlib
import hashlib
import io
import pathlib
import sys
import tempfile
import tomllib
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from r3d.import_settings import LIGHT_FIELDS, SettingsError, albedo_jobs, load_import_settings, load_scene
from r3d import build_pack
from anim_probe import write_camera_clip  # noqa: E402

try:
    import numpy as np
    from tests import soup  # noqa: F401  (traces the bake's rays on the scalar variant)

    from r3d import mesh_import
    from r3d.light import LIGHTS, encode_srgb8, light, to_srgb8
    from r3d.lit_mesh import read_lit_mesh
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = '[source]\npath = "m.obj"\ncredit = "c"\n'
OUTPUT = '[output]\ndirectory = "."\nname = "mesh"\n'
AMBIENT = '[ambient]\ncolor = [1.0, 1.0, 1.0]\nintensity = 0.1\n'
TONEMAP = 'tonemap_white = 0.3\n'
SIMPLIFY = '[geometry]\nsimplify = { dense_edge = 1.0, props = [], props_share = 0.3, seal_seams = true, colour_deviation = 1.0 }\n'
VARIANT = '[[variants]]\nname = "mesh"\ntriangles = 10\n'
BAKE = '[bake]\nray_offset = 0.5\ncolour_merge_step = 6\n'
HEAD = TONEMAP + AMBIENT + BAKE
PLAIN_VARIANT = '[[variants]]\nname = "mesh"\n'
VARIANT_OUTPUT = '[output]\ndirectory = "."\n'
FLAT_FIT = ('fit = { budget = 8, train_every_ms = 1000, held_out_every_ms = 5000, coverage_every_ms = 100, steps = 20, '
            'batch = 4, laplacian = 10.0, normal_weight = 1.0, sha256 = "ab", recipe_sha256 = "cd" }\n')
FIT = ('[objects.mesh_renderer.fit.prune]\nbudget = 8\ncoverage_every_ms = 100\n'
       '[objects.mesh_renderer.fit.poses]\ntrain_every_ms = 1000\nheld_out_every_ms = 5000\n'
       '[objects.mesh_renderer.fit.optimise]\nsteps = 20\nbatch = 4\nlaplacian = 10.0\nnormal_weight = 1.0\n'
       '[objects.mesh_renderer.fit.hashes]\nsha256 = "ab"\nrecipe_sha256 = "cd"\n')
INDIRECT = 'indirect = { bounces = 2, rays = 8 }\n'
THIN = '[geometry]\nthin = { material = "m", keep = 0.5 }\n'
REGION = 'visibility = { source = "camera_region", rounds = 2 }\n'
PATH = 'visibility = { source = "camera_path", every_ms = 100, size = [8, 6], margin = 2 }\n'
CUBE = ("v 0 0 0\nv 8 0 0\nv 8 8 0\nv 0 8 0\nv 0 0 8\nv 8 0 8\nv 8 8 8\nv 0 8 8\nusemtl m\n"
        "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n")


def write_import(directory, name="mesh.import.toml", source=SOURCE, output=OUTPUT, body=""):
    path = pathlib.Path(directory) / name
    path.write_text(source + output + body)
    if source == SOURCE:
        for name, content in (("m.obj", CUBE), ("m.mtl", "newmtl m\nKd 1 1 1\n")):
            asset = path.parent / name
            if not asset.exists():
                asset.write_text(content)
    return path


def write_scene(directory, objects, head="", name="scene.scene.toml"):
    """The scene file; beside it fly.anim.toml, the clip camera(path=True) names."""
    path = pathlib.Path(directory) / name
    path.write_text(head + objects)
    if not (path.parent / "fly.anim.toml").exists():
        write_camera_clip(path.parent)
    return path


def renderer(mesh="mesh.import.toml", extra="", transform="", name=None):
    name = name or mesh.split(".")[0]
    return f'[[objects]]\nname = "{name}"\n{transform}[objects.mesh_renderer]\nmesh = "{mesh}"\n{extra}'


def sun_object(rotation="[0.0, 0.0, 0.0]"):
    return (f'[[objects]]\nname = "sun"\nrotation = {rotation}\n[objects.light]\ntype = "directional"\n'
            'color = [1.0, 1.0, 1.0]\nintensity = 1.0\n')


def camera(path=False, region=True):
    track = 'path = { animation = "fly.anim.toml", node = "camera" }\n' if path else ''
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


    def test_basic_settings_turn_no_step_on(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory))
        self.assertEqual([variant.name for variant in settings.variants], ["mesh"])
        self.assertEqual((settings.alpha_keep, settings.thin, settings.simplify), (None,) * 3)

    def test_a_present_step_table_turns_the_step_on(self):
        body = '[process]\nseed = 2\n[geometry]\nalpha_mask = { keep_alpha = 0.5 }\nthin = { material = "m", keep = 0.5 }\n'
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory, body=body))
        self.assertEqual((settings.alpha_keep, settings.thin.keep, settings.seed), (0.5, 0.5, 2))

    def test_the_output_directory_is_type_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(SettingsError, "output.directory"):
                load_import_settings(write_import(directory, output='[output]\ndirectory = 3\nname = "mesh"\n'))

    def test_the_committed_settings_and_scenes_load(self):
        scenes = tree_scenes()
        self.assertTrue(scenes)
        for path in scenes:
            self.assertTrue(load_scene(path).renderers, path.name)

    def test_an_unknown_key_is_rejected_in_a_group(self):
        self.rejects("process.typo", "[process.typo]\nx = 1\n")
        self.rejects("geometry.typo", "[geometry]\ntypo = 1\n")

    def test_an_unknown_key_is_rejected_in_each_nested_group(self):
        self.rejects("geometry.alpha_mask.typo", "[geometry]\nalpha_mask = { keep_alpha = 0.5, typo = 1 }\n")
        self.rejects("geometry.thin.typo", '[geometry]\nthin = { material = "m", keep = 0.5, typo = 1 }\n')
        self.rejects("geometry.simplify.typo", SIMPLIFY.replace(" }", ", typo = 1 }") + VARIANT)

    def test_a_source_without_a_path_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(SettingsError, "source.path"):
                load_import_settings(write_import(directory, source=SOURCE.replace('path = "m.obj"\n', "")))

    def test_an_unknown_key_is_rejected_in_a_table(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(SettingsError, "typo"):
                load_import_settings(write_import(directory, source=SOURCE + "typo = 1\n"))

    def test_a_flag_and_a_count_are_type_checked(self):
        self.rejects("true or false", SIMPLIFY.replace("seal_seams = true", 'seal_seams = "no"') + VARIANT)
        self.rejects("must be an integer", SIMPLIFY + VARIANT.replace("= 10", "= 1.5"))

    def test_simplify_needs_a_positive_colour_deviation(self):
        self.rejects("colour_deviation is required", SIMPLIFY.replace(", colour_deviation = 1.0", "") + VARIANT)
        for value in ("0", "-1.0"):
            self.rejects("must be above 0", SIMPLIFY.replace("colour_deviation = 1.0", f"colour_deviation = {value}") + VARIANT)
        with tempfile.TemporaryDirectory() as directory:
            body = SIMPLIFY.replace("colour_deviation = 1.0", "colour_deviation = 0.01") + VARIANT
            settings = load_import_settings(write_import(directory, output='[output]\ndirectory = "."\n', body=body))
        self.assertEqual(settings.simplify.colour_deviation, 0.01)

    def test_simplify_needs_a_budget_per_variant_and_a_budget_needs_simplify(self):
        self.rejects("needs variants", SIMPLIFY)
        self.rejects("triangles is required", SIMPLIFY + PLAIN_VARIANT)
        self.rejects("needs geometry.simplify", VARIANT)

    def test_the_legacy_process_layout_names_each_options_new_home(self):
        self.rejects(r"\[process.light\] moved to scene \[bake\]", "[process.light]\nray_offset = 0.5\n")
        self.rejects(r"\[process.visibility\] moved to objects\.mesh_renderer\.visibility", "[process.visibility]\nrounds = 2\n")

    def test_the_tools_name_no_scene(self):
        banned = set()
        files = build_pack.input_files([build_pack.DEFAULT_SEARCH, build_pack.DEMO])
        for path in (file for file in files if file.name.endswith(build_pack.IMPORT)):
            values = tomllib.loads(path.read_text())
            banned.update(variant["name"].lower() for variant in values.get("variants", []))
            if "name" in values["output"]:
                banned.add(values["output"]["name"].lower())
        banned.update(path.name.removesuffix(".scene.toml").lower()
                      for path in files if path.name.endswith(build_pack.SCENE))
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

    def plain_import(self, directory):
        write_import(directory)

    def lit(self, extra="", head=HEAD, body="", output=OUTPUT, objects=""):
        """Rejects/loads one baked renderer of a mesh import with `body`, lit by one sun."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        write_import(directory.name, output=output, body=body)
        return write_scene(directory.name, renderer(extra="bake = true\n" + extra) + sun_object() + objects, head)

    def lit_rejects(self, pattern, extra="", **kwargs):
        with self.assertRaisesRegex(SettingsError, pattern):
            load_scene(self.lit(extra, **kwargs))

    def test_a_scene_with_two_mesh_renderers_validates(self):
        with tempfile.TemporaryDirectory() as directory:
            self.two_imports(directory)
            scene = load_scene(write_scene(directory, renderer("a.import.toml")
                                           + renderer("b.import.toml", transform="position = [1.0, 2.0, 3.0]\nscale = [2.0, 2.0, 2.0]\n")))
        self.assertEqual([item.renderer.variant.name for item in scene.renderers], ["a", "b"])
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

    def test_object_names_are_unique(self):
        self.rejects("names must be unique", self.two_imports, renderer("a.import.toml") + renderer("b.import.toml", name="a"))

    def test_a_variant_is_chosen_from_the_import_that_has_them(self):
        def setup(directory):
            write_import(directory, output=VARIANT_OUTPUT, body=PLAIN_VARIANT)

        self.rejects("variant is required", setup, renderer())
        self.rejects("not in", setup, renderer(extra='variant = "other"\n'))
        self.rejects("has no variants", self.two_imports, renderer("a.import.toml", 'variant = "a"\n'))

    def test_a_baked_renderer_needs_what_it_reads_from_the_scene(self):
        self.lit_rejects("tonemap_white is required", head=AMBIENT + BAKE)
        self.lit_rejects("camera region is required", REGION, objects=camera(region=False))

    def test_scene_settings_no_placed_mesh_reads_are_rejected(self):
        self.rejects("lights is read by no placed mesh", self.two_imports, renderer("a.import.toml"), AMBIENT)
        self.rejects("lights is read by no placed mesh", self.two_imports, renderer("a.import.toml") + sun_object())
        self.rejects("tonemap_white is read by no placed mesh", self.two_imports, renderer("a.import.toml"), TONEMAP)
        self.rejects("camera region is read by no placed mesh", self.two_imports, renderer("a.import.toml") + camera())

    def test_visibility_reads_its_source(self):
        scene = load_scene(self.lit(REGION + PATH.replace("visibility = ", "#"), objects=camera()))
        region = scene.renderers[0].renderer.visibility
        self.assertEqual((region.source, region.rounds), ("camera_region", 2))
        scene = load_scene(self.lit(PATH, head=HEAD, objects=camera(path=True, region=False)))
        path = scene.renderers[0].renderer.visibility
        self.assertEqual((path.source, path.every_ms, path.size, path.samples, path.margin), ("camera_path", 100, (8, 6), 3, 2))
        self.lit_rejects("visibility.source", 'visibility = { source = "navmesh", rounds = 2 }\n')
        self.lit_rejects("visibility", 'visibility = { source = "camera_path", rounds = 2 }\n')
        self.lit_rejects("margin cannot be negative", PATH.replace("margin = 2", "margin = -1"))

    def test_path_visibility_needs_the_camera_path_not_its_region(self):
        self.lit_rejects("camera path is required", PATH, objects=camera(region=False))
        self.lit_rejects("camera region is read by no placed mesh", PATH, objects=camera(path=True))

    def test_each_renderer_of_one_import_has_its_own_visibility(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            scene = load_scene(write_scene(
                directory, renderer(extra="bake = true\n" + REGION) + renderer(extra="bake = true\n", name="other")
                + sun_object() + camera(), HEAD))
        self.assertEqual([item.renderer.visibility is not None for item in scene.renderers], [True, False])

    def test_a_fit_recipe_reads_and_needs_a_bake_and_room_to_prune(self):
        extra = 'variant = "mesh"\n' + FIT
        scene = load_scene(self.lit(extra, body=SIMPLIFY + VARIANT, output=VARIANT_OUTPUT))
        fit = scene.renderers[0].renderer.fit
        self.assertEqual((fit.budget, fit.normal_weight, fit.sha256), (8, 1.0, "ab"))
        options = {"body": SIMPLIFY + VARIANT, "output": VARIANT_OUTPUT}
        self.lit_rejects("cannot exceed", extra.replace("budget = 8", "budget = 11"), **options)
        self.lit_rejects("fit", extra.replace("steps = 20\n", ""), **options)
        self.lit_rejects("fit.prune.typo", extra.replace("budget = 8", "budget = 8\ntypo = 1"), **options)
        self.lit_rejects(r"fit\.budget moved to .*fit\.prune\.budget", 'variant = "mesh"\n' + FLAT_FIT, **options)
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory, output=VARIANT_OUTPUT, body=SIMPLIFY + VARIANT)
            with self.assertRaisesRegex(SettingsError, "bake = true is required"):
                load_scene(write_scene(directory, renderer(extra=extra)))

    def test_indirect_light_needs_complete_nonnegative_settings(self):
        base = "colour_merge_step = 6"
        for pattern, bounced in (("bake.indirect.bounces", "{ bounces = -1, rays = 8 }"),
                                 ("bake.indirect.bounces", "{ bounces = 0, rays = 8 }"),
                                 ("bake.indirect.rays", "{ bounces = 1, rays = 0 }"),
                                 ("bake.indirect.rays is required", "{ bounces = 1 }")):
            with self.subTest(pattern=pattern):
                self.lit_rejects(pattern, head=TONEMAP + AMBIENT + BAKE + f"indirect = {bounced}\n")
        scene = load_scene(self.lit(head=HEAD + INDIRECT))
        self.assertEqual((scene.bake.indirect.bounces, scene.bake.indirect.rays), (2, 8))
        self.assertTrue(base in BAKE)

    def test_local_occlusion_is_off_unless_asked_for_and_validated_strictly(self):
        self.assertIsNone(load_scene(self.lit(head=HEAD)).bake.ao)
        scene = load_scene(self.lit(head=HEAD + "ao = { distance = 40.0, rays = 16 }\n"))
        self.assertEqual(vars(scene.bake.ao), {"distance": 40.0, "rays": 16, "strength": 1.0, "indirect": False})
        self.assertEqual(vars(scene.renderers[0].bake.ao), vars(scene.bake.ao))
        for pattern, table in (("ao.distance must be positive", "{ distance = 0.0, rays = 16 }"),
                               ("ao.rays", "{ distance = 4.0, rays = 0 }"),
                               ("ao.strength must be between", "{ distance = 4.0, rays = 8, strength = 1.5 }"),
                               ("ao.rays is required", "{ distance = 4.0 }"),
                               ("ao.depth", "{ distance = 4.0, rays = 8, depth = 1 }")):
            with self.subTest(pattern=pattern):
                self.lit_rejects(pattern, head=HEAD + f"ao = {table}\n")

    def test_local_occlusion_needs_something_to_scale(self):
        head = TONEMAP + BAKE + "ao = { distance = 4.0, rays = 8 }\n"
        self.lit_rejects("has neither", head=head)
        self.assertTrue(load_scene(self.lit(head=head.replace("ao = {", "ao = { indirect = true,") + INDIRECT)).bake.ao.indirect)
        for dead in (head.replace("ao = {", "ao = { indirect = true,"), HEAD + "ao = { indirect = true, distance = 4.0, rays = 8 }\n"):
            self.lit_rejects("read by no placed mesh|has neither", head=dead)
        self.lit_rejects("read by no placed mesh", "indirect = false\n",
                         head=HEAD + INDIRECT + "ao = { indirect = true, distance = 4.0, rays = 8 }\n")

    def test_a_renderer_can_leave_the_indirect_light_out(self):
        head = HEAD + INDIRECT
        self.lit_rejects("indirect can only be false", "indirect = true\n", head=head)
        self.lit_rejects("needs scene.bake.indirect", "indirect = false\n")
        scene = load_scene(self.lit("indirect = false\n", head=head))
        self.assertFalse(scene.renderers[0].renderer.indirect)
        self.assertEqual(scene.bake.indirect.bounces, 2)

    def test_shading_defaults_to_smooth_and_flat_keeps_its_samples(self):
        flat = "shading = { flat = { fixed = 4 } }\n"
        smooth = load_scene(self.lit()).renderers[0].renderer
        explicit = load_scene(self.lit('shading = "smooth"\n')).renderers[0].renderer
        shaded = load_scene(self.lit(flat)).renderers[0].renderer
        self.assertIsNone(smooth.face_samples)
        self.assertIsNone(explicit.face_samples)
        self.assertEqual(shaded.face_samples, (4, 1, 4, None))

    def test_auto_samples_with_a_minimum_over_its_maximum_are_rejected(self):
        shading = 'shading = { flat = { auto = { min = 3, max = 2, area = "median" } } }\n'
        self.lit_rejects("min", shading)

    def test_the_indirect_look_defaults_to_physical_and_is_validated_strictly(self):
        bounced = HEAD + INDIRECT
        tuned_table = "[indirect]\nintensity = 2.5\nalbedo_boost = 1.5\n"
        plain = load_scene(self.lit(head=bounced))
        tuned = load_scene(self.lit(head=bounced + tuned_table))
        self.assertEqual((plain.indirect.intensity, plain.indirect.albedo_boost), (1.0, 1.0))
        self.assertEqual((tuned.indirect.intensity, tuned.indirect.albedo_boost), (2.5, 1.5))
        for pattern, table in (("scene.indirect.intensity must not be negative", "intensity = -1\n"),
                               ("scene.indirect.albedo_boost must be positive", "albedo_boost = 0\n"),
                               ("scene.indirect.intensity must be a number", 'intensity = "2"\n'),
                               ("scene.indirect.gain is not a known setting", "gain = 2\n")):
            self.lit_rejects(pattern, head=bounced + "[indirect]\n" + table)
        self.lit_rejects("indirect settings is read by no placed mesh", head=HEAD + tuned_table)

    def test_a_baked_mesh_is_baked_where_it_sits(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        write_import(directory.name)
        placed = renderer(extra="bake = true\n", transform="position = [1.0, 0.0, 0.0]\n") + sun_object()
        with self.assertRaisesRegex(SettingsError, "identity"):
            load_scene(write_scene(directory.name, placed, HEAD))
        self.two_imports(directory.name)
        load_scene(write_scene(directory.name, renderer("a.import.toml", transform="position = [1.0, 0.0, 0.0]\n")))

    def test_a_directional_lights_direction_comes_from_its_rotation(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            objects = renderer(extra="bake = true\n")
            overhead = load_scene(write_scene(directory, objects + sun_object(), HEAD)).lights[0]["direction"]
            turned = load_scene(write_scene(directory, objects + sun_object("[90.0, 90.0, 0.0]"), HEAD)).lights[0]["direction"]
        for got, want in ((overhead, [0.0, 1.0, 0.0]), (turned, [1.0, 0.0, 0.0])):
            self.assertTrue(all(abs(a - b) < 1e-12 for a, b in zip(got, want)), got)

    def test_the_lights_are_the_directional_objects_then_the_sky_then_the_ambient(self):
        sky = '[sky]\ncolor = [0.5, 0.5, 0.5]\nintensity = 1.0\nrays = 2\n'
        scene = load_scene(self.lit(head=HEAD + sky))
        self.assertEqual([item["type"] for item in scene.lights], ["directional", "sky", "ambient"])

    def test_the_first_camera_carries_the_bake_lens_and_region(self):
        scene = load_scene(self.lit(REGION, objects=camera()))
        self.assertEqual(scene.camera.component.near_z, 1.0)
        self.assertEqual(scene.region[1], [1.0, 1.0, 1.0])
        self.rejects("first camera", self.two_imports, renderer("a.import.toml") + camera(region=False)
                     + camera().replace('"camera"', '"other"'))
        self.rejects("half_fov_short_tan", self.two_imports, renderer("a.import.toml")
                     + '[[objects]]\nname = "c"\n[objects.camera]\nnear_z = 1.0\n')
        self.rejects("letters, digits and _", self.two_imports, renderer("a.import.toml")
                     + camera(path=True, region=False).replace('node = "camera"', 'node = "no-good"'))

    def test_an_unknown_key_is_rejected_in_a_light_and_in_the_sky(self):
        self.rejects("typo", self.two_imports, renderer("a.import.toml") + sun_object().replace("intensity = 1.0\n", "intensity = 1.0\ntypo = 1\n"))
        self.lit_rejects("typo", head=HEAD + "[sky]\ncolor = [1.0, 1.0, 1.0]\nintensity = 1.0\nrays = 1\ntypo = 1\n")

    def test_a_reserved_light_type_is_rejected(self):
        point = '[[objects]]\nname = "p"\n[objects.light]\ntype = "point"\n'
        self.rejects("reserved", self.two_imports, renderer("a.import.toml") + point)

    def test_sky_and_ambient_are_not_light_objects(self):
        sky = '[[objects]]\nname = "s"\n[objects.light]\ntype = "sky"\n'
        self.rejects("scene settings", self.two_imports, renderer("a.import.toml") + sky)

    def test_a_scale_must_be_positive(self):
        self.rejects("positive", self.two_imports, renderer("a.import.toml", transform="scale = [1.0, -1.0, 1.0]\n"))
        self.rejects("positive", self.two_imports, renderer("a.import.toml", transform="scale = [0.0, 1.0, 1.0]\n"))

    def test_two_objects_of_one_scene_cannot_share_a_name(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            with self.assertRaisesRegex(SettingsError, "names must be unique"):
                load_scene(write_scene(directory, renderer() + renderer(name="mesh")))

    def test_a_scene_run_is_stamped_by_what_it_places(self):
        from r3d.fitted_variant import recipe_digest

        extra = 'variant = "mesh"\n' + FIT
        fly = camera(path=True, region=False)

        def digest(head=HEAD, fit=extra, reach=1.0, simplify=SIMPLIFY):
            with tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                write_camera_clip(root, reach=reach)
                write_import(root, output=VARIANT_OUTPUT, body=simplify + VARIANT)
                scene = load_scene(write_scene(root, renderer(extra="bake = true\n" + PATH + fit) + sun_object() + fly, head))
                job = scene.renderers[0]
                return recipe_digest(job, scene)

        first = digest()
        self.assertEqual(digest(fit=extra.replace('sha256 = "ab"', 'sha256 = "ef"')), first, "the recorded hashes are not the recipe")
        self.assertNotEqual(digest(fit=extra.replace("steps = 20", "steps = 21")), first)
        self.assertNotEqual(digest(head=HEAD.replace("ray_offset = 0.5", "ray_offset = 0.6")), first)
        self.assertNotEqual(digest(reach=2.0), first, "the camera clip's keys are the recipe")
        self.assertNotEqual(digest(simplify=SIMPLIFY.replace("dense_edge = 1.0", "dense_edge = 2.0")), first)
        self.assertNotEqual(digest(head=HEAD.replace("intensity = 0.1", "intensity = 0.2")), first, "the scene's lights are the recipe")
        self.assertNotEqual(digest(head=HEAD.replace("tonemap_white = 0.3", "tonemap_white = 0.4")), first)
        self.assertNotEqual(digest(head=HEAD + INDIRECT + "[indirect]\nintensity = 2.0\n"), digest(head=HEAD + INDIRECT))
        self.assertNotEqual(digest(head=HEAD + "ao = { distance = 4.0, rays = 8 }\n"), first, "occlusion is part of the recipe")

    def test_the_recipe_digest_drops_indirect_for_an_opted_out_renderer(self):
        from r3d.fitted_variant import recipe_digest

        def digest(bounces, opted_out=True):
            out = "indirect = false\n" if opted_out else ""
            with tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                write_import(root, output=VARIANT_OUTPUT, body=SIMPLIFY + VARIANT)
                head = HEAD + INDIRECT.replace("bounces = 2", f"bounces = {bounces}")
                scene = load_scene(write_scene(
                    root, renderer(extra='variant = "mesh"\nbake = true\n' + out + PATH + FIT) + sun_object()
                    + camera(path=True, region=False), head))
                job = scene.renderers[0]
                return recipe_digest(job, scene)

        self.assertEqual(digest(2), digest(3))
        self.assertNotEqual(digest(2, opted_out=False), digest(3, opted_out=False))

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

    def test_the_indirect_option_needs_its_scene_bake_knob(self):
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            objects = renderer(extra='bake = true\nindirect = false\n') + sun_object()
            with self.assertRaisesRegex(SettingsError, "needs scene.bake.indirect"):
                load_scene(write_scene(directory, objects, TONEMAP + BAKE))

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
                "colour_merge_step = 6", "colour_merge_step = 6\nindirect = { bounces = 1, rays = 2 }")
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
        self.assertEqual((scene.camera.component.path.clip, scene.camera.component.path.node), ("fly", "camera"))

    def test_identical_import_variant_budgets_are_rejected(self):
        variants = VARIANT + VARIANT.replace('name = "mesh"', 'name = "copy"')
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(SettingsError, "same mesh"):
                load_import_settings(write_import(directory, output='[output]\ndirectory = "."\n', body=SIMPLIFY + variants))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class AuthoredImportTests(unittest.TestCase):
    def bake(self, directory, scene=None, output=OUTPUT, head="", name="mesh.scene.toml"):
        """Runs the importer on an import, or on the scene whose objects are `scene`."""
        root = pathlib.Path(directory)
        (root / "m.obj").write_text(CUBE)
        (root / "m.mtl").write_text("newmtl m\nKd 0.5 0.25 0.125\n")
        import_path = write_import(root, output=output)
        path = write_scene(root, scene, head, name=name) if scene else import_path
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
            self.assertEqual(self.bake(directory, renderer()), 0)
            mesh = read_lit_mesh(pathlib.Path(directory) / "mesh.mesh")
        self.assertEqual(len(mesh.tris), 12)

    def test_an_albedo_bake_runs_no_step(self):
        steps = ("drop_masked", "visible_from_region", "light", "simplify", "densify", "face_colours",
                 "merge_matching_colours")
        for scene in (None, renderer()):
            with self.subTest(scene=bool(scene)), tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
                for name in steps:
                    stack.enter_context(mock.patch(f"r3d.mesh_import.{name}", side_effect=AssertionError(name)))
                self.assertEqual(self.bake(directory, scene), 0)
                mesh = read_lit_mesh(pathlib.Path(directory) / "mesh.mesh")
            self.assertEqual((len(mesh.tris), len(mesh.pos)), (12, 8))

    def test_an_import_writes_its_mesh_beside_the_import_file_not_in_the_output_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            pathlib.Path(directory, "generated").mkdir()
            self.assertEqual(self.bake(directory, output='[output]\ndirectory = "generated"\nname = "mesh"\n'), 0)
            beside = (pathlib.Path(directory) / "mesh.mesh").is_file()
            elsewhere = list(pathlib.Path(directory, "generated").iterdir())
        self.assertTrue(beside)
        self.assertEqual(elsewhere, [])

    @needs_mitsuba
    def test_a_baked_renderer_writes_its_mesh_beside_the_scene_named_by_scene_and_object(self):
        objects = renderer(extra="bake = true\n") + sun_object()
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.bake(directory, objects, head=HEAD, name="hall.scene.toml"), 0)
            written = sorted(item.name for item in pathlib.Path(directory).glob("*.mesh"))
            lit = read_lit_mesh(pathlib.Path(directory) / "hall.mesh.mesh")
        self.assertEqual(written, ["hall.mesh.mesh"])
        target = 255.0 * np.array([0.5, 0.25, 0.125])
        self.assertFalse((np.abs(lit.rgb - target) <= 8).all(), "a lit mesh is not its albedo")


class ClearIntersector:
    def blocked(self, origins, directions):
        return np.zeros(len(origins), dtype=bool)


def sun(direction, color, intensity):
    return {"type": "directional", "direction": direction, "color": color, "intensity": intensity}


def radiance(lights, normal, double_sided):
    return light(np.array([[0.0, 0.0, 0.0]]), np.array([normal]), np.array([double_sided]), ClearIntersector(),
                 lights, 0.5)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class LightListTests(unittest.TestCase):
    def test_directional_lights_add_their_radiance_and_order_does_not_matter(self):
        lights = [
            {"type": "directional", "direction": [0, 1, 0], "color": [1, 0, 0], "intensity": 2},
            {"type": "directional", "direction": [0, 1, 0], "color": [0, 1, 0], "intensity": 3},
        ]
        args = (np.array([[0.0, 0.0, 0.0]]), np.array([[0.0, 1.0, 0.0]]), np.array([False]), ClearIntersector())
        first = light(*args, lights, 0.5)
        np.testing.assert_allclose(first, [[2.0, 3.0, 0.0]])
        np.testing.assert_allclose(light(*args, lights[::-1], 0.5), first)

    def test_a_double_sided_face_turns_to_the_light(self):
        got = radiance([sun([0, 1, 0], [1, 1, 1], 1)], [0.0, -1.0, 0.0], True)
        np.testing.assert_allclose(got, [[1.0, 1.0, 1.0]])

    def test_the_order_of_every_light_type_does_not_change_the_radiance(self):
        lights = [sun([0, 1, 0], [1, 0, 0], 2), sun([1, 0.1, 0], [0, 1, 0], 1),
                  {"type": "sky", "color": [0, 0, 1], "intensity": 1, "rays": 1},
                  {"type": "ambient", "color": [1, 1, 1], "intensity": 0.1}]
        for normal in ([0.0, -1.0, 0.0], [-1.0, 0.0, 0.0], [0.0, 1.0, 0.0]):
            for double_sided in (False, True):
                forward = radiance(lights, normal, double_sided)
                np.testing.assert_allclose(radiance(lights[::-1], normal, double_sided), forward)

    def test_the_unlit_encoder_is_the_tone_mapped_one_without_a_tone_map(self):
        linear = np.array([0.0, 0.1, 0.5, 1.0, 2.0])
        np.testing.assert_array_equal(encode_srgb8(linear), to_srgb8(linear, 0.0))
        self.assertEqual(encode_srgb8(np.array([0.5]))[0], round(255 * 0.5 ** (1 / 2.2)))

    def test_the_light_reader_and_bakers_agree(self):
        self.assertEqual(set(LIGHTS), set(LIGHT_FIELDS))


class DigestTests(unittest.TestCase):
    def test_the_recipe_digest_ignores_equivalent_toml_layouts(self):
        from r3d.fitted_variant import recipe_digest

        fit = FIT
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_import(root, output='[output]\ndirectory = "."\n', body=SIMPLIFY + VARIANT)
            objects = renderer(extra='variant = "mesh"\nbake = true\nvisibility = { source = "camera_path", every_ms = 10, size = [8, 6] }\n' + fit)
            first = load_scene(write_scene(root, objects + sun_object() + camera(path=True, region=False), TONEMAP + AMBIENT + BAKE))
            alternate = (TONEMAP + AMBIENT + '[bake]\ncolour_merge_step = 6\nray_offset = 0.5\n')
            second = load_scene(write_scene(root, objects + sun_object() + camera(path=True, region=False), alternate,
                                            "other.scene.toml"))
            first_digest = recipe_digest(first.renderers[0], first)
            second_digest = recipe_digest(second.renderers[0], second)
        self.assertEqual(first_digest, second_digest)


class FitLayoutTests(unittest.TestCase):
    def test_checked_in_fit_recipes_match_their_recorded_digest(self):
        from r3d.fitted_variant import recipe_digest

        for path in tree_scenes():
            scene = load_scene(path)
            for job in scene.renderers:
                if job.renderer.fit:
                    self.assertEqual(recipe_digest(job, scene), job.renderer.fit.recipe_sha256,
                                     job.asset_path.name)


def triangle_source(p):
    """A loaded source of one untextured, uncoloured triangle at the corners p."""
    return SimpleNamespace(colors=None, p=p, uv=None, tri_v=np.array([[0, 1, 2]]), tri_t=None, tri_m=np.array([0]),
                           names=["m"], textures=[None], materials={})


@unittest.skipIf(np is None, "the r3d environment is not installed")
class BakeStepTests(unittest.TestCase):
    def test_the_bake_culls_with_the_renderers_visibility_only(self):
        own = SimpleNamespace(source="camera_path")
        settings = SimpleNamespace(seed=1, position_scale=None, alpha_keep=None, thin=None, simplify=None, double_sided=set())
        source = triangle_source(np.zeros((3, 3)))
        seen = []
        with mock.patch.object(mesh_import, "load_source", return_value=source), \
                mock.patch.object(mesh_import, "RayQuery"), \
                mock.patch.object(mesh_import, "visible_triangles", side_effect=lambda v, *rest: seen.append(v) or np.array([True])), \
                mock.patch.object(mesh_import, "shade_unlit",
                                  return_value=(np.zeros((3, 3)), np.zeros((3, 3)), np.array([[0, 1, 2]]))):
            for visibility in (own, None):
                renderer_ = SimpleNamespace(visibility=visibility, variant=SimpleNamespace(triangles=None))
                mesh_import.bake_geometry(SimpleNamespace(settings=settings, renderer=renderer_, bake=None), None)
        self.assertEqual(seen, [own])

    def test_the_simplifier_gets_the_imports_colour_deviation(self):
        steps = SimpleNamespace(dense_edge=10.0, props=set(), props_share=0.3, seal_seams=True, colour_deviation=2.5)
        settings = SimpleNamespace(seed=1, position_scale=None, alpha_keep=None, thin=None, simplify=steps, double_sided=set())
        p = np.array([[0.0, 0, 0], [1, 0, 0], [0, 1, 0]])
        source = triangle_source(p)
        given = {}

        def simplify(pos, rgb, tris, labels, *rest, **options):
            given.update(options)
            return pos, rgb, tris, labels

        with mock.patch.object(mesh_import, "load_source", return_value=source), \
                mock.patch.object(mesh_import, "shade_unlit", return_value=(p, np.zeros((3, 3)), np.array([[0, 1, 2]]))), \
                mock.patch.object(mesh_import, "simplify", side_effect=simplify):
            renderer_ = SimpleNamespace(visibility=None, variant=SimpleNamespace(triangles=1))
            mesh_import.bake_geometry(SimpleNamespace(settings=settings, renderer=renderer_, bake=None), None)
        self.assertEqual(given["colour_deviation"], 2.5)

    def test_check_fitted_names_what_changed_and_what_to_do(self):
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "m.mesh"
            target.write_bytes(b"mesh")
            renderer_ = SimpleNamespace(variant=SimpleNamespace(name="m"), fit=SimpleNamespace(sha256="0", recipe_sha256="r"))
            job = SimpleNamespace(renderer=renderer_, asset_path=target)
            with mock.patch("r3d.fitted_variant.recipe_digest", return_value="other"):
                with self.assertRaisesRegex(SystemExit, "recipe changed since the fit; rerun fitted_variant.py prepare[|]fit"):
                    mesh_import.check_fitted(job, None)
            with mock.patch("r3d.fitted_variant.recipe_digest", return_value="r"):
                with self.assertRaisesRegex(SystemExit, "not the 0 the fit recorded; rerun fitted_variant.py"):
                    mesh_import.check_fitted(job, None)
                job.asset_path = target.with_name("gone.mesh")
                with self.assertRaisesRegex(SystemExit, "gone.mesh is missing"):
                    mesh_import.check_fitted(job, None)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ReferenceObjectTests(unittest.TestCase):
    def test_a_reference_is_lit_as_its_own_object_is_baked(self):
        from r3d import reference_render

        bounced = HEAD + INDIRECT
        objects = (renderer(extra="bake = true\n", name="bounced") + renderer(extra="bake = true\nindirect = false\n", name="dark")
                   + sun_object())
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            scene = load_scene(write_scene(directory, objects, bounced))
            source = triangle_source(np.zeros((3, 3)))
            built = []
            with mock.patch.object(reference_render, "load_source", return_value=source), \
                    mock.patch.object(reference_render, "RayQuery"), \
                    mock.patch.object(mesh_import, "PathLight", side_effect=lambda *args: built.append(args) or "cache"):
                _, dark = reference_render.source_for(scene, "dark")
                lit, bounced_job = reference_render.source_for(scene, "bounced")
                with self.assertRaisesRegex(ValueError, "no mesh renderer named"):
                    reference_render.source_for(scene, "sun")
        self.assertEqual(dark.object.name, "dark")
        self.assertEqual((len(built), lit.bounce), (1, "cache"))
        self.assertEqual(bounced_job.bake.indirect.bounces, 2)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class PathLightForTests(unittest.TestCase):
    def setUp(self):
        mesh_import.PATH_LIGHTS.clear()
        self.addCleanup(mesh_import.PATH_LIGHTS.clear)

    def job(self, indirect):
        return SimpleNamespace(bake=SimpleNamespace(indirect=indirect), settings=SimpleNamespace(path="a.import.toml", double_sided=set()))

    def test_a_renderer_with_no_indirect_builds_no_scene(self):
        with mock.patch.object(mesh_import, "PathLight") as built:
            self.assertIsNone(mesh_import.path_light_for(None, self.job(None), SimpleNamespace()))
        built.assert_not_called()

    def test_one_scene_is_shared_by_equal_settings_and_only_the_latest_is_kept(self):
        indirect = SimpleNamespace(bounces=1, rays=8)
        scene = lambda boost, lights: SimpleNamespace(lights=lights, indirect=SimpleNamespace(albedo_boost=boost, intensity=1.0))
        with mock.patch.object(mesh_import, "PathLight", side_effect=lambda *args: object()) as built:
            first = mesh_import.path_light_for(None, self.job(indirect), scene(1.0, []))
            self.assertIs(mesh_import.path_light_for(None, self.job(indirect), scene(1.0, [])), first)
            self.assertEqual(built.call_count, 1)
            mesh_import.path_light_for(None, self.job(indirect), scene(2.0, []))
        self.assertEqual(built.call_count, 2)
        self.assertEqual(len(mesh_import.PATH_LIGHTS), 1)

    def test_the_intensity_does_not_rebuild_the_scene(self):
        indirect = SimpleNamespace(bounces=1, rays=8)
        scene = lambda intensity: SimpleNamespace(lights=[], indirect=SimpleNamespace(albedo_boost=1.0, intensity=intensity))
        with mock.patch.object(mesh_import, "PathLight", side_effect=lambda *args: object()) as built:
            mesh_import.path_light_for(None, self.job(indirect), scene(1.0))
            mesh_import.path_light_for(None, self.job(indirect), scene(3.0))
        self.assertEqual(built.call_count, 1)


class JobTests(unittest.TestCase):
    def test_a_job_carries_the_scene_bake_its_renderer_uses(self):
        smooth = 'bake = true\nvariant = "mesh"\n'
        head = HEAD + INDIRECT
        flat = "shading = { flat = { fixed = 4 } }\n"
        with tempfile.TemporaryDirectory() as directory:
            write_import(directory)
            objects = (renderer(extra="bake = true\n", name="full") + renderer(extra="bake = true\nindirect = false\n", name="dark")
                       + renderer(extra="bake = true\n" + flat, name="flat") + renderer(name="plain") + sun_object())
            scene = load_scene(write_scene(directory, objects, head))
        full, dark, flat_job, plain = scene.renderers
        self.assertEqual(full.bake.indirect.bounces, 2)
        self.assertIsNone(dark.bake.indirect)
        self.assertEqual(flat_job.bake.indirect.bounces, 2)
        self.assertIsNone(plain.bake)
        self.assertEqual((full.bake.ray_offset, full.bake.colour_merge_step), (0.5, 6))

    def test_a_bare_import_has_an_albedo_job_per_variant(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory, output=VARIANT_OUTPUT, body=SIMPLIFY + VARIANT))
            jobs = albedo_jobs(settings)
        self.assertEqual([(job.asset_name, job.bake, job.object, job.renderer.bake) for job in jobs],
                         [("mesh", None, None, False)])
        self.assertEqual(jobs[0].asset_path, settings.mesh_dir / "mesh.mesh")
        self.assertEqual(set(vars(jobs[0])), {"settings", "renderer", "object", "asset_name", "asset_path", "bake"})


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
                    self.assertEqual(hashlib.sha256(job.asset_path.read_bytes()).hexdigest(), job.renderer.fit.sha256,
                                     job.asset_path.name)
        self.assertGreater(found, 0)


class LocalSourceTests(unittest.TestCase):
    def test_lfs_pointer_oid_and_malformed_pointers(self):
        from r3d.import_settings import content_checksum, lfs_pointer_oid

        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "m.obj"
            path.write_bytes(b"geometry")
            self.assertIsNone(lfs_pointer_oid(path))
            oid = hashlib.sha256(path.read_bytes()).hexdigest().encode()
            for newline in (b"\n", b"\r\n"):
                with self.subTest(newline=newline):
                    path.write_bytes(newline.join((b"version https://git-lfs.github.com/spec/v1",
                                                   b"oid sha256:" + oid, b"size 8", b"")))
                    self.assertEqual(lfs_pointer_oid(path), oid)
                    self.assertEqual(content_checksum(path), oid)
            for record in (b"", b"oid sha256:invalid", b"oid sha256:" + oid + b"\noid sha256:" + oid):
                with self.subTest(record=record):
                    path.write_bytes(b"version https://git-lfs.github.com/spec/v1\n" + record + b"\nsize 8\n")
                    with self.assertRaisesRegex(SettingsError, r"m\.obj.*malformed.*LFS"):
                        content_checksum(path)

    @unittest.skipIf(np is None, "the r3d environment is not installed")
    def test_load_source_rejects_unpulled_files_before_loading(self):
        pointer = b"version https://git-lfs.github.com/spec/v1\noid sha256:" + b"a" * 64 + b"\nsize 8\n"
        for name in ("m.obj", "m.mtl", "colour.png", "alpha.png"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                settings = load_import_settings(write_import(directory))
                root = pathlib.Path(directory)
                (root / "m.mtl").write_text("newmtl m\nmap_Kd colour.png\nmap_d alpha.png\n")
                for texture in ("colour.png", "alpha.png"):
                    (root / texture).write_bytes(b"hydrated texture")
                (root / name).write_bytes(pointer)
                with mock.patch.object(mesh_import, "load_obj") as load_obj, \
                        mock.patch.object(mesh_import, "load_textures") as load_textures:
                    with self.assertRaises(SettingsError) as error:
                        mesh_import.load_source(settings)
                    self.assertIn(str(root / name), str(error.exception))
                    self.assertIn('git lfs pull --exclude=""', str(error.exception))
                    load_obj.assert_not_called()
                    load_textures.assert_not_called()

    def test_path_is_relative_to_import(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write_import(directory, source='[source]\npath = "asset/m.obj"\ncredit = "c"\n')
            settings = load_import_settings(path)
            self.assertEqual(settings.source["path"], (path.parent / "asset/m.obj").resolve())

    def test_unsupported_format_names_obj(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write_import(directory, source='[source]\npath = "m.zip"\ncredit = "c"\n')
            with self.assertRaisesRegex(SettingsError, r"supported.*\.obj"):
                load_import_settings(path)

    def test_download_keys_are_rejected(self):
        for key in ("url", "sha256", "cache"):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as directory:
                source = '[source]\npath = "m.obj"\ncredit = "c"\n' + key + ' = "x"\n'
                with self.assertRaisesRegex(SettingsError, key + " is not a known setting"):
                    load_import_settings(write_import(directory, source=source))

    def test_source_changes_invalidate_recipe_and_reference(self):
        from r3d.fitted_variant import recipe_digest, reference_digest

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            asset = root / "asset"
            asset.mkdir()
            (asset / "m.obj").write_text("geometry")
            (asset / "m.mtl").write_text("newmtl m\nmap_Kd texture.weird\n")
            (asset / "texture.weird").write_bytes(b"texture")
            source = '[source]\npath = "asset/m.obj"\ncredit = "c"\n'
            write_import(root, source=source, output='[output]\ndirectory = "."\n', body=SIMPLIFY + VARIANT)
            objects = renderer(extra='variant = "mesh"\nbake = true\n' + FIT)
            scene_path = write_scene(root, objects + sun_object() + camera(path=True, region=False), TONEMAP + AMBIENT + BAKE)
            scene = load_scene(scene_path)
            job = scene.renderers[0]
            for name in ("m.obj", "m.mtl", "texture.weird"):
                before = recipe_digest(job, scene), reference_digest(scene_path, job, scene)
                path = asset / name
                path.write_bytes(path.read_bytes() + b"\n# changed")
                after = recipe_digest(job, scene), reference_digest(scene_path, job, scene)
                self.assertTrue(all(a != b for a, b in zip(before, after)), name)

    def test_the_camera_clip_s_keys_and_nothing_else_of_its_file_stamp_recipe_and_reference(self):
        from r3d.fitted_variant import recipe_digest, reference_digest

        def digests(**clip):
            with tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                write_camera_clip(root, **clip)
                write_import(root, output=VARIANT_OUTPUT, body=SIMPLIFY + VARIANT)
                scene_path = write_scene(root, renderer(extra='variant = "mesh"\nbake = true\n' + PATH + FIT)
                                         + sun_object() + camera(path=True, region=False), HEAD)
                scene = load_scene(scene_path)
                job = scene.renderers[0]
                return recipe_digest(job, scene), reference_digest(scene_path, job, scene)

        first = digests()
        for changed in (digests(reach=2.0), digests(degrees=30.0)):
            self.assertTrue(all(a != b for a, b in zip(first, changed)), "a key of the clip changed")
        self.assertEqual(digests(props=("unused",)), first, "the .glb changed but not the clip it bakes to")

    def test_doc_stamp_covers_source_directory(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))
        import doc_stages
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            import_path = root / "launcher/m.import.toml"
            import_path.parent.mkdir()
            asset = import_path.parent / "asset"
            asset.mkdir()
            (asset / "m.obj").write_text("geometry")
            (asset / "m.mtl").write_text("newmtl m\nmap_Kd texture.weird\n")
            texture = asset / "texture.weird"
            texture.write_bytes(b"texture")
            write_import(import_path.parent, name=import_path.name, source='[source]\npath = "asset/m.obj"\ncredit = "c"\n')
            with mock.patch.object(doc_stages, "ROOT", root), mock.patch.object(doc_stages.subprocess, "check_output", return_value=b"launcher/m.import.toml\0"):
                before = doc_stages.current_stamp()
                texture.write_bytes(b"new texture")
                self.assertNotEqual(doc_stages.current_stamp(), before)


if __name__ == "__main__":
    unittest.main()
