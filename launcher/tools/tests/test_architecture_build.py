"""architecture/build.py: header summaries, components, and the built page."""

import json
import pathlib
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "architecture"))

import build  # noqa: E402


class Summary(unittest.TestCase):
    def test_the_first_sentence_of_the_top_comment_without_its_name_lead(self):
        cases = (
            ("/* gfx: the one framebuffer. Drawing lives here. */\n", "The one framebuffer."),
            ("/*\n * job_queue (core 1) - work handed across. More.\n */\n", "Work handed across."),
            ("// tween: easing in one place.\n// Second line.\nint x;\n", "Easing in one place."),
            ("/* ======\n * GENERATED FILE - do not edit.\n */\n", "Generated file."),
            ("#include <stdio.h>\n/* not a top comment */\n", ""),
            ("/* " + "word " * 80 + "end. */", None),
        )
        for text, expected in cases:
            with self.subTest(text=text[:30]):
                got = build.summary(text)
                if expected is None:
                    self.assertLessEqual(len(got), 200)
                    self.assertTrue(got.endswith("..."))
                else:
                    self.assertEqual(got, expected)


class Components(unittest.TestCase):
    def test_a_file_belongs_to_its_folder_its_app_the_shell_or_the_root_headers(self):
        cases = (
            ("launcher/main/gfx/gfx.c", "gfx"),
            ("launcher/main/util/math/vec2f.h", "util"),
            ("launcher/main/apps/sand/ui/brush_screen.c", "apps/sand"),
            ("launcher/main/main.c", "shell"),
            ("launcher/main/app_registry.c", "shell"),
            ("launcher/main/app.h", "contract"),
            ("launcher/main/app_arena.c", "contract"),
            ("launcher/main/build_variant.h", "contract"),
            ("launcher/test/suites/suite_gfx.c", "suites"),
        )
        for rel, expected in cases:
            with self.subTest(rel=rel):
                self.assertEqual(build.component_of(rel), expected)


class Built(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = build.build_data()
        cls.by_id = {c["id"]: c for c in cls.data["components"]}

    def test_every_layer_in_the_architecture_diagram_is_a_described_component(self):
        doc = (build.ROOT / "docs/Firmware-Architecture.md").read_text(encoding="utf-8")
        layers = {m.group(1): m for m in build.LAYER_LABEL.finditer(doc) if m.group(1) != "apps"}
        self.assertGreater(len(layers), 5)
        for name, match in layers.items():
            with self.subTest(layer=name):
                self.assertIn(name, self.by_id)
                self.assertEqual(self.by_id[name]["desc"], match.group(2) + ".")
                self.assertEqual(self.by_id[name]["hw"], bool(match.group(3)))

    def test_every_edge_joins_known_components_and_counts_at_least_one_include(self):
        known = set(self.by_id) | {"ext:ESP-IDF", "ext:microui"}
        for edge in self.data["edges"]:
            with self.subTest(edge=edge):
                self.assertIn(edge["from"], self.by_id)
                self.assertIn(edge["to"], known)
                self.assertNotEqual(edge["from"], edge["to"])
                self.assertGreaterEqual(edge["n"], 1)

    def test_embedded_data_cannot_end_its_script_and_reads_back_whole(self):
        cases = ({"summary": "</script><b>x</b>"}, {"path": "a/b.c", "n": 3}, ["</", "<\\/"])
        for data in cases:
            with self.subTest(data=data):
                text = build.embed(data)
                self.assertNotIn("</", text)
                self.assertEqual(json.loads(text), data)

    def test_the_page_carries_the_data_once(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(build.main(["-o", out]), 0)
            page = (pathlib.Path(out) / "index.html").read_text(encoding="utf-8")
        self.assertNotIn("__DATA__", page)
        found = re.search(r"const DATA = (\{.*?\});\n", page, re.S)
        self.assertIsNotNone(found)
        self.assertNotIn("</", found.group(1))
        self.assertEqual(json.loads(found.group(1).replace("<\\/", "</"))["source"],
                         self.data["source"])


if __name__ == "__main__":
    unittest.main()
