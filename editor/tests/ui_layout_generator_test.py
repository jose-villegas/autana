"""Generator output and shared invalid-layout fixture tests."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
GENERATOR_PATH = ROOT / "launcher" / "tools" / "gen" / "gen_ui_layout.py"
UI_DIR = ROOT / "launcher" / "main" / "ui"

SPEC = importlib.util.spec_from_file_location("gen_ui_layout", GENERATOR_PATH)
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class UiLayoutGeneratorTest(unittest.TestCase):
    def setUp(self):
        self.layout = json.loads((UI_DIR / "control_center_layout.json").read_text(encoding="utf-8"))

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

    def test_small_passive_element_is_accepted(self):
        rect = self.layout["orientations"]["portrait"]["rects"]["notifications_header"]
        self.assertLess(rect[3], GENERATOR.MIN_TAP_TARGET)
        GENERATOR.validate(self.layout)

    def test_bad_documents_are_rejected(self):
        fixtures = Path(__file__).parent / "fixtures" / "layout_document"
        paths = sorted(fixtures.glob("*.json"))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(fixture=path.name):
                with self.assertRaises(ValueError):
                    GENERATOR.validate(json.loads(path.read_text(encoding="utf-8")))


if __name__ == "__main__":
    unittest.main()
