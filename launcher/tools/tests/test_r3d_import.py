"""Checks import settings, the scene lighting interface and that the tools name no scene."""

import pathlib
import sys
import tempfile
import tomllib
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.import_settings import LIGHT_FIELDS, SettingsError, load_import_settings  # noqa: E402

try:
    import numpy as np

    from r3d.light import LIGHTS, light
except ImportError:
    np = None

ROOT = pathlib.Path(__file__).resolve().parents[3]

SOURCE = '[source]\nurl = "https://example.invalid/m.zip"\nsha256 = "abc"\npath = "m.obj"\ncache = "m"\ncredit = "c"\n'
VARIANT = '[[variants]]\nname = "mesh"\ntriangles = 100\nflat = false\n'
LIGHTS_TOML = '[[lights]]\ntype = "ambient"\ncolor = [1.0, 1.0, 1.0]\nintensity = 0.1\n'


def options(extra=""):
    return ("[options]\nmask_keep_alpha = 0.5\nvisibility_rounds = 1\nleaf_keep = 1.0\nseed = 1\nray_offset = 0.5\n"
            f"position_scale = 8\ncolour_merge_step = 6\ndense_edge = 1.0\nprops_share = 0.3\nseal_seams = true\n{extra}")


def write_settings(directory, source=SOURCE, variants=VARIANT, lights=LIGHTS_TOML, extra=""):
    directory = pathlib.Path(directory)
    (directory / "scene.toml").write_text(
        f"tonemap_white = 0.35\n[camera_region]\nmin = [0.0, 0.0, 0.0]\nmax = [1.0, 1.0, 1.0]\n{lights}")
    path = directory / "mesh.import.toml"
    path.write_text('scene = "scene.toml"\n' + source + '[output]\ndirectory = "."\n'
                    '[materials]\ndouble_sided = []\nprops = []\nleaf_material = "leaf"\n' + options(extra) + variants)
    return path


class SettingsTests(unittest.TestCase):
    def rejects(self, pattern, **changes):
        with tempfile.TemporaryDirectory() as directory:
            path = write_settings(directory, **changes)
            with self.assertRaisesRegex(SettingsError, pattern):
                load_import_settings(path)

    def test_a_valid_settings_file_loads(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_import_settings(write_settings(directory))
        self.assertEqual([variant.name for variant in settings.variants], ["mesh"])

    def test_the_committed_settings_load(self):
        found = sorted(ROOT.glob("launcher/main/apps/*/meshes/*.import.toml"))
        self.assertTrue(found)
        for path in found:
            self.assertTrue(load_import_settings(path).variants, path.name)

    def test_auto_samples_with_a_minimum_over_its_maximum_are_rejected(self):
        variants = VARIANT.replace("flat = false", 'flat = true\nface_samples = { auto = { min = 3, max = 2, area = "median" } }')
        self.rejects("min", variants=variants, extra="flat_sky_rays = 8\n")

    def test_a_source_without_a_sha_is_rejected(self):
        self.rejects("sha256", source=SOURCE.replace('sha256 = "abc"\n', ""))

    def test_an_unknown_key_is_rejected_in_a_table_and_in_a_light(self):
        self.rejects("typo", source=SOURCE + "typo = 1\n")
        self.rejects("typo", lights=LIGHTS_TOML + "typo = 1\n")

    def test_a_zero_light_direction_is_rejected(self):
        lights = ('[[lights]]\ntype = "directional"\ndirection = [0.0, 0.0, 0.0]\ncolor = [1.0, 1.0, 1.0]\n'
                  "intensity = 1.0\ndisc_degrees = 1.0\nrays = 1\n")
        self.rejects("zero", lights=lights)

    def test_a_flag_and_a_count_are_type_checked(self):
        self.rejects("true or false", variants=VARIANT.replace("flat = false", 'flat = "no"'))
        self.rejects("must be an integer", variants=VARIANT.replace("triangles = 100", "triangles = 1.5"))

    def test_a_missing_option_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write_settings(directory)
            path.write_text(path.read_text().replace("dense_edge = 1.0\n", ""))
            with self.assertRaisesRegex(SettingsError, "options.dense_edge is required"):
                load_import_settings(path)

    def test_the_flat_sky_rays_belong_to_a_flat_variant(self):
        self.rejects("flat variant", extra="flat_sky_rays = 8\n")

    def test_a_reserved_light_type_is_rejected(self):
        self.rejects("reserved", lights='[[lights]]\ntype = "point"\n')

    def test_the_tools_name_no_scene(self):
        banned = set()
        for path in ROOT.glob("launcher/main/apps/**/meshes/*.import.toml"):
            banned.update(variant["name"].lower() for variant in tomllib.loads(path.read_text())["variants"])
        banned.update(path.name.removesuffix(".scene.toml").lower()
                      for path in ROOT.glob("launcher/main/apps/**/meshes/*.scene.toml"))
        self.assertTrue(banned)
        for path in sorted((ROOT / "launcher/tools/r3d").rglob("*")):
            if path.suffix in (".py", ".md", ".txt") and ".cache" not in path.parts:
                content = path.read_text().lower()
                for name in banned:
                    self.assertNotIn(name, content, path.name)


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
