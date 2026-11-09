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
    def settings(self, text):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        path = pathlib.Path(folder.name) / "app.render.toml"
        path.write_text(text, encoding="utf-8")
        return path

    def test_a_file_changes_only_its_keys(self):
        self.assertEqual(render_asset.load(self.settings("batched = false\n")),
                         {"cull": True, "depth_fit": True, "batched": False, "partial_redraw": True, "lod.tolerance_px": 2.0,
                          "lod.hysteresis_px": 0.5, "lod.coverage_permille": 250, "lod.pick_every": 4,
                          "lod.triangle_budget_k": 0})
        lod = render_asset.load(self.settings("[lod]\ncoverage_permille = 100\n"))
        self.assertEqual(lod["lod.coverage_permille"], 100)

    def test_the_bake_is_one_row_per_field_in_struct_order(self):
        rows, frac = table_asset.decode(render_asset.bake(self.settings("cull = false\n")))
        self.assertEqual((list(rows.items()), frac), ([("cull", [0]), ("depth_fit", [16]), ("batched", [16]),
                                                  ("partial_redraw", [16]), ("lod.tolerance_px", [32]), ("lod.hysteresis_px", [8]),
                                                  ("lod.coverage_permille", [4000]), ("lod.pick_every", [64]),
                                                  ("lod.triangle_budget_k", [0])], 4))

    def test_a_frame_s_triangle_budget_bakes_in_thousands(self):
        # A row holds 2047 at most; a budget of tens of thousands of triangles fits only in thousands.
        rows, frac = table_asset.decode(render_asset.bake(self.settings("[lod]\ntriangle_budget_k = 32\n")))
        self.assertEqual(rows["lod.triangle_budget_k"], [32 << frac])

    def test_an_unknown_key_or_another_type_is_refused(self):
        for text, message in (("fog = true\n", "fog is not a render setting"),
                              ("cull = 1\n", "cull must be a TOML bool"),
                              ("[lod]\nfog = 1\n", "lod.fog is not a render setting"),
                              ("[lod]\ncoverage_permille = 1.5\n", "lod.coverage_permille must be a TOML int"),
                              ("cull = \n", "app.render.toml")):
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, message):
                render_asset.load(self.settings(text))

    def test_a_value_between_the_tables_steps_is_refused(self):
        for text in ("[lod]\ntolerance_px = 0.1\n", "[lod]\ncoverage_permille = 3000\n"):
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, "not a multiple of 1/16"):
                render_asset.bake(self.settings(text))

    def test_each_field_needs_its_own_line_and_tier_comment(self):
        for body in ("bool cull, batched; /* basic: x */", "bool cull;"):
            with self.subTest(body=body), self.assertRaisesRegex(ValueError, "its own line"):
                render_asset.fields(f"typedef struct {{\n    {body}\n}} render_settings_t;")

    def test_the_generated_default_matches_the_tree(self):
        header = render_asset.SETTINGS / "render_settings_default.h"
        self.assertEqual(header.read_text(encoding="utf-8"), render_asset.header())


if __name__ == "__main__":
    unittest.main()
