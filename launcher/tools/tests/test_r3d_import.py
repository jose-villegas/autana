"""Checks import settings, scene files, the lighting interface and that the tools name no scene."""

import contextlib
import pathlib
import sys
import tempfile
import tomllib
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.import_settings import LIGHT_FIELDS, SettingsError, load_import_settings, load_scene  # noqa: E402

try:
    import numpy as np

    from r3d import mesh_import
    from r3d.light import LIGHTS, light
    from r3d.lit_mesh import read_lit_mesh
except ImportError:
    np = None

ROOT = pathlib.Path(__file__).resolve().parents[3]

SOURCE = '[source]\nurl = "https://example.invalid/m.zip"\nsha256 = "abc"\npath = "m.obj"\ncache = "m"\ncredit = "c"\n'
OUTPUT = '[output]\ndirectory = "."\nname = "mesh"\n'
LIGHTS_TOML = '[[lights]]\ntype = "ambient"\ncolor = [1.0, 1.0, 1.0]\nintensity = 0.1\n'
LIGHT_STEP = "[process.light]\nray_offset = 0.5\ncolour_merge_step = 6\n"
SIMPLIFY_STEP = '[process.simplify]\ndense_edge = 1.0\nprops = []\nprops_share = 0.3\nseal_seams = true\n'
VARIANT = '[[variants]]\nname = "mesh"\n'
CUBE = ("v 0 0 0\nv 8 0 0\nv 8 8 0\nv 0 8 0\nv 0 0 8\nv 8 0 8\nv 8 8 8\nv 0 8 8\nusemtl m\n"
        "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n")


def write_import(directory, name="mesh.import.toml", source=SOURCE, output=OUTPUT, body=""):
    path = pathlib.Path(directory) / name
    path.write_text(source + output + body)
    return path


def write_scene(directory, renderers, body="", name="scene.toml"):
    path = pathlib.Path(directory) / name
    path.write_text(renderers + body)
    return path


def renderer(mesh="mesh.import.toml", extra=""):
    return f'[[mesh_renderers]]\nmesh = "{mesh}"\n{extra}'


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
        body = "[process.alpha_mask]\nkeep_alpha = 0.5\n[process.visibility]\nrounds = 4\n" + LIGHT_STEP
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_import(directory, body=body))
        self.assertEqual((settings.alpha_keep, settings.visibility.rounds, settings.light.ray_offset), (0.5, 4, 0.5))

    def test_the_committed_settings_and_scenes_load(self):
        scenes = sorted(ROOT.glob("launcher/main/apps/*/meshes/*.scene.toml"))
        self.assertTrue(scenes)
        for path in scenes:
            self.assertTrue(load_scene(path).renderers, path.name)

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
    def rejects(self, pattern, setup, scene_body):
        with tempfile.TemporaryDirectory() as directory:
            setup(directory)
            path = write_scene(directory, scene_body)
            with self.assertRaisesRegex(SettingsError, pattern):
                load_scene(path)

    def two_imports(self, directory):
        write_import(directory, "a.import.toml", output='[output]\ndirectory = "."\nname = "a"\n')
        write_import(directory, "b.import.toml", output='[output]\ndirectory = "."\nname = "b"\n')

    def test_a_scene_with_two_mesh_renderers_validates(self):
        with tempfile.TemporaryDirectory() as directory:
            self.two_imports(directory)
            scene = load_scene(write_scene(directory, renderer("a.import.toml")
                                           + renderer("b.import.toml", "position = [1.0, 2.0, 3.0]\nscale = [2.0, 2.0, 2.0]\n")))
        self.assertEqual([item.variant.name for item in scene.renderers], ["a", "b"])
        self.assertEqual(scene.renderers[0].position, [0.0, 0.0, 0.0])
        self.assertEqual(scene.renderers[1].position, [1.0, 2.0, 3.0])

    def test_a_renderer_naming_a_missing_import_file_is_rejected(self):
        self.rejects("not a file", self.two_imports, renderer("a.import.toml") + renderer("gone.import.toml"))

    def test_a_scene_without_renderers_or_with_an_unknown_key_is_rejected(self):
        self.rejects("mesh_renderers", self.two_imports, LIGHTS_TOML)
        self.rejects("typo", self.two_imports, renderer("a.import.toml") + "typo = 1\n")

    def test_a_mesh_name_is_baked_once_per_scene(self):
        self.rejects("twice", self.two_imports, renderer("a.import.toml") + renderer("a.import.toml"))

    def test_a_variant_is_chosen_from_the_import_that_has_them(self):
        def setup(directory):
            write_import(directory, output='[output]\ndirectory = "."\n', body=VARIANT)

        self.rejects("variant is required", setup, renderer())
        self.rejects("not in", setup, renderer(extra='variant = "other"\n'))
        self.rejects("has no variants", self.two_imports, renderer("a.import.toml", 'variant = "a"\n'))

    def test_a_light_baked_mesh_needs_the_scene_lights_and_an_identity_transform(self):
        def setup(directory):
            write_import(directory, body=LIGHT_STEP)

        self.rejects("lights is required", setup, renderer())
        self.rejects("exposure is required", setup, renderer() + LIGHTS_TOML)
        self.rejects("identity", setup, renderer(extra="position = [1.0, 0.0, 0.0]\n") + LIGHTS_TOML + "exposure = 0.3\n")

    def test_the_visibility_step_needs_the_camera_region(self):
        def setup(directory):
            write_import(directory, body="[process.visibility]\nrounds = 2\n")

        self.rejects("camera_region is required", setup, renderer())

    def test_a_zero_light_direction_is_rejected(self):
        lights = ('[[lights]]\ntype = "directional"\ndirection = [0.0, 0.0, 0.0]\ncolor = [1.0, 1.0, 1.0]\n'
                  "intensity = 1.0\ndisc_degrees = 1.0\nrays = 1\n")
        self.rejects("zero", self.two_imports, renderer("a.import.toml") + lights)

    def test_an_unknown_key_is_rejected_in_a_light(self):
        self.rejects("typo", self.two_imports, renderer("a.import.toml") + LIGHTS_TOML + "typo = 1\n")

    def test_a_reserved_light_type_is_rejected(self):
        self.rejects("reserved", self.two_imports, renderer("a.import.toml") + '[[lights]]\ntype = "point"\n')


@unittest.skipIf(np is None, "the r3d environment is not installed")
class AuthoredImportTests(unittest.TestCase):
    def bake(self, directory, body=""):
        (pathlib.Path(directory) / "m.obj").write_text(CUBE)
        (pathlib.Path(directory) / "m.mtl").write_text("newmtl m\nKd 0.5 0.25 0.125\n")
        write_import(directory, body=body)
        scene = write_scene(directory, renderer())
        with mock.patch("r3d.mesh_import.fetch_zip", return_value=pathlib.Path(directory)), \
                mock.patch("r3d.mesh_import.REPO", pathlib.Path(directory)):
            return mesh_import.main([str(scene)])

    def test_basic_settings_keep_the_source_triangles_and_run_no_step(self):
        steps = ("drop_masked", "visible_from_region", "light", "simplify", "densify", "face_colours",
                 "merge_matching_colours")
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
            for name in steps:
                stack.enter_context(mock.patch(f"r3d.mesh_import.{name}", side_effect=AssertionError(name)))
            self.assertEqual(self.bake(directory), 0)
            mesh = read_lit_mesh(pathlib.Path(directory) / "mesh_mesh_generated.c")
        self.assertEqual(len(mesh.tris), 12)
        self.assertEqual(len(mesh.pos), 8)

    def test_unlit_colour_is_the_material_albedo(self):
        with tempfile.TemporaryDirectory() as directory:
            self.bake(directory)
            mesh = read_lit_mesh(pathlib.Path(directory) / "mesh_mesh_generated.c")
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

    def test_every_light_type_has_a_baker(self):
        self.assertEqual(set(LIGHTS), set(LIGHT_FIELDS))


if __name__ == "__main__":
    unittest.main()
