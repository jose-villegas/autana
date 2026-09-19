import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
GENERATOR_PATH = ROOT / "launcher" / "tools" / "gen_ui_layout.py"
UI_DIR = ROOT / "launcher" / "main" / "ui"

SPEC = importlib.util.spec_from_file_location("gen_ui_layout", GENERATOR_PATH)
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class UiLayoutGeneratorTest(unittest.TestCase):
    def setUp(self):
        self.layout = json.loads((UI_DIR / "control_center_layout.json").read_text(encoding="utf-8"))

    def assert_rejected(self, layout, message):
        with self.assertRaisesRegex(ValueError, message):
            GENERATOR.validate(layout)

    def test_every_checked_in_header_matches_its_source(self):
        sources = sorted(UI_DIR.glob("*_layout.json"))
        self.assertTrue(sources)
        for source in sources:
            with self.subTest(source=source.name):
                baked = GENERATOR.generate(json.loads(source.read_text(encoding="utf-8")))
                header = source.with_name(source.stem + "_generated.h")
                self.assertEqual(baked, header.read_text(encoding="utf-8"))

    def test_identifiers_derive_from_the_screen_name(self):
        layout = copy.deepcopy(self.layout)
        layout["screen"] = "settings"
        baked = GENERATOR.generate(layout)
        self.assertIn("SETTINGS_ELEMENT_WIFI = 0,", baked)
        self.assertIn("} settings_layout_t;", baked)
        self.assertIn("static const settings_layout_t settings_layout_landscape = {", baked)
        self.assertNotIn("control_center", baked.lower())

    def test_overlap_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["orientations"]["portrait"]["rects"]["bluetooth"] = [16, 54, 160, 78]
        self.assert_rejected(layout, "overlaps")

    def test_small_interactive_element_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["orientations"]["landscape"]["rects"]["volume"][3] = 43
        self.assert_rejected(layout, "44px tap target")

    def test_small_passive_element_is_accepted(self):
        header = self.layout["orientations"]["portrait"]["rects"]["notifications_header"]
        self.assertLess(header[3], 44)
        GENERATOR.validate(self.layout)

    def test_rect_outside_the_canvas_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["orientations"]["portrait"]["rects"]["wifi"][0] = 300
        self.assert_rejected(layout, "leaves the canvas")

    def test_rects_must_match_the_declared_elements(self):
        layout = copy.deepcopy(self.layout)
        del layout["orientations"]["portrait"]["rects"]["link"]
        self.assert_rejected(layout, "exactly the declared element ids")

    def test_repeated_element_id_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["elements"].append(copy.deepcopy(layout["elements"][0]))
        self.assert_rejected(layout, "repeated")

    def test_element_id_must_be_a_c_identifier(self):
        layout = copy.deepcopy(self.layout)
        layout["elements"][0]["id"] = "Wi-Fi"
        self.assert_rejected(layout, "lower_snake_case")

    def test_old_schema_is_rejected(self):
        layout = copy.deepcopy(self.layout)
        layout["schema_version"] = 1
        self.assert_rejected(layout, "schema_version")


if __name__ == "__main__":
    unittest.main()
