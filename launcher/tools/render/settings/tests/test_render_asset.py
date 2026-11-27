"""render_asset reads a .render.toml over default.render.toml, refuses what render_settings_t lacks,
and bakes the TABL rows render_settings_read() takes."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import render_asset  # noqa: E402
from asset import table_asset  # noqa: E402


class RenderAssetTests(unittest.TestCase):
    def settings(self, text, name="app.render.toml"):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        path = pathlib.Path(folder.name) / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_a_file_changes_only_its_keys(self):
        self.assertEqual(render_asset.load(self.settings("batched = false\n")),
                         {"cull": True, "depth_fit": True, "batched": False, "partial_redraw": True, "lod.tolerance_px": 2.0,
                          "lod.hysteresis_px": 0.5, "lod.coverage_permille": 250, "lod.pick_every": 4,
                          "lod.triangle_budget": 0, "lod.blend": 0, "lod.blend_px": 0.25}
                         | {key: value for key, value in render_asset.read(render_asset.DEFAULT).items()
                            if key.startswith(("anim.", "texture.", "small_fill.", "settle."))})
        lod = render_asset.load(self.settings("[lod]\ncoverage_permille = 100\n"))
        self.assertEqual(lod["lod.coverage_permille"], 100)

    def test_the_bake_is_one_row_per_field_in_struct_order(self):
        rows, frac = table_asset.decode(render_asset.bake(self.settings("cull = false\n")))
        self.assertEqual((list(rows.items()), frac), ([("cull", [0]), ("depth_fit", [1]), ("batched", [1]),
                                                  ("partial_redraw", [1]), ("lod.tolerance_px", [32]), ("lod.hysteresis_px", [8]),
                                                  ("lod.coverage_permille", [250]), ("lod.pick_every", [4]),
                                                  ("lod.triangle_budget", [0]), ("lod.blend", [0]), ("lod.blend_px", [4])]
                                                  + [(key, [int(value * (1 << render_asset.FRAC_BITS))
                                                            if render_asset.fields()[key][0] == "float" else value])
                                                     for key, value in render_asset.read(render_asset.DEFAULT).items()
                                                     if key.startswith(("anim.", "texture.", "small_fill.", "settle."))],
                                                 4))

    def test_an_int_bakes_as_itself_up_to_a_row_s_most(self):
        for budget in (12000, table_asset.VALUE_MAX):
            rows, _ = table_asset.decode(render_asset.bake(self.settings(f"[lod]\ntriangle_budget = {budget}\n")))
            self.assertEqual(rows["lod.triangle_budget"], [budget])
        with self.assertRaisesRegex(ValueError, f"from {table_asset.VALUE_MIN} to {table_asset.VALUE_MAX}"):
            render_asset.bake(self.settings(f"[lod]\ntriangle_budget = {table_asset.VALUE_MAX + 1}\n"))

    def test_an_unknown_key_or_another_type_is_refused(self):
        for text, message in (("fog = true\n", "fog is not a render setting"),
                              ("cull = 1\n", "cull must be a TOML bool"),
                              ("[lod]\nfog = 1\n", "lod.fog is not a render setting"),
                              ("[lod]\ncoverage_permille = 1.5\n", "lod.coverage_permille must be a TOML int"),
                              ("cull = \n", "app.render.toml")):
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, message):
                render_asset.load(self.settings(text))

    def test_a_value_between_the_tables_steps_is_refused(self):
        for text in ("[lod]\ntolerance_px = 0.1\n", "[lod]\ntolerance_px = 2048.0\n"):
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, "not a multiple of 1/16"):
                render_asset.bake(self.settings(text))

    def test_each_field_needs_its_own_line_and_tier_comment(self):
        for body in ("bool cull, batched; /* basic: x */", "bool cull;"):
            with self.subTest(body=body), self.assertRaisesRegex(ValueError, "its own line"):
                render_asset.fields(f"typedef struct {{\n    {body}\n}} render_settings_t;")

    def test_generated_fields_carry_the_interval_their_comment_gives(self):
        header = render_asset.header()
        self.assertIn("X(lod.blend, int, value >= 0 && value < RENDER_LOD_BLEND_COUNT)", header)
        self.assertIn("X(lod.blend_px, float, value > 0.0F && value <= RENDER_LOD_BLEND_PX_MAX)", header)
        self.assertIn("X(lod.tolerance_px, float, true)", header)
        source = render_asset.HEADER.read_text(encoding="utf-8").replace(
            "float tolerance_px;  /* basic:", "float tolerance_px;  /* basic [1.0, 2.5):")
        self.assertEqual(render_asset.fields(source)["lod.tolerance_px"][3], "value >= 1.0F && value < 2.5F")

    def test_an_interval_bound_must_be_a_number_or_a_c_constant(self):
        for bound in ("max", "1.2.3"):  # each passes the tier comment's shape and fails BOUND
            source = render_asset.HEADER.read_text(encoding="utf-8").replace(
                "float tolerance_px;  /* basic:", f"float tolerance_px;  /* basic [0.0, {bound}]:")
            with self.subTest(bound=bound), self.assertRaisesRegex(
                    ValueError, f"tolerance_px's bound '{bound}' is not a number or a C constant"):
                render_asset.fields(source)

    def test_an_interval_is_the_c_test_its_brackets_and_bounds_give(self):
        for kind, bounds, test in (("float", ("[", "-2", "-0.5", "]"), "value >= -2.0F && value <= -0.5F"),
                                   ("float", ("(", "0", "8", ")"), "value > 0.0F && value < 8.0F"),
                                   ("float", ("(", "0", "RANGE_MAX", "]"), "value > 0.0F && value <= RANGE_MAX"),
                                   ("int", ("[", "-3", "4", ")"), "value >= -3 && value < 4"),
                                   ("int", ("[", "0", "C_COUNT", ")"), "value >= 0 && value < C_COUNT"),
                                   ("float", None, "true")):
            with self.subTest(bounds=bounds):
                self.assertEqual(render_asset.interval(kind, bounds, "field"), test)

    def test_an_interval_that_cannot_hold_its_field_is_refused(self):
        for kind, bounds, message in (("bool", ("[", "0", "1", "]"), "is a bool and takes no interval"),
                                      ("int", ("[", "0", "1.5", "]"), "is an int and its bound 1.5 is not"),
                                      ("float", ("[", "5", "2", "]"), "starts above its end"),
                                      ("float", ("[", "-1", "-2.5", "]"), "starts above its end")):
            with self.subTest(bounds=bounds), self.assertRaisesRegex(ValueError, message):
                render_asset.interval(kind, bounds, "field")

    def test_a_bound_is_its_constants_c_value(self):
        for bound, value in (("RENDER_LOD_BLEND_PX_MAX", 4.0), ("SMALL_FILL_STYLE_COUNT", 3.0),
                             ("RENDER_LOD_BLEND_COUNT", 2.0), ("-2", -2.0)):
            with self.subTest(bound=bound):
                self.assertEqual(render_asset.constant(bound), value)
        self.assertTrue(render_asset.within(2, ("[", "0", "SMALL_FILL_STYLE_COUNT", ")")))
        self.assertFalse(render_asset.within(3, ("[", "0", "SMALL_FILL_STYLE_COUNT", ")")))
        with self.assertRaisesRegex(ValueError, "NO_SUCH is not a numeric #define or an enum member"):
            render_asset.constant("NO_SUCH")

    def test_a_file_value_outside_its_fields_interval_is_refused(self):
        path = self.settings("[lod]\nblend_px = 1000.0\n")
        with self.assertRaisesRegex(ValueError, "lod.blend_px = 1000.0 is outside"):
            render_asset.read(path)
        with self.assertRaisesRegex(ValueError, "lod.blend_px = 0.0 is outside"):
            render_asset.read(self.settings("[lod]\nblend_px = 0.0\n"))
        self.assertEqual(render_asset.read(self.settings("[lod]\nblend_px = 0.5\n"))["lod.blend_px"], 0.5)

    def test_the_generated_default_matches_the_tree(self):
        header = render_asset.SETTINGS / "render_settings_default.h"
        self.assertEqual(header.read_text(encoding="utf-8"), render_asset.header())


if __name__ == "__main__":
    unittest.main()
