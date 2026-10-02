"""Regression tests for scripts/gates/check_comment_layers.py."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import check_comment_layers  # noqa: E402


class ProblemsTest(unittest.TestCase):
    """One tree per case: `files` maps a repo-relative path to its text."""

    def problems(self, files, apps=("sand", "render_lab", "diagnostics")):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            for app in apps:
                (root / "launcher/main/apps" / app).mkdir(parents=True)
            for path, text in files.items():
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")
            return check_comment_layers.problems(str(root))

    def test_a_lower_layer_comment_naming_an_app_fails(self):
        found = self.problems({"launcher/main/gfx/gfx.c": "/* sand calls this */\nint x;\n"})
        self.assertEqual(found, ["launcher/main/gfx/gfx.c:1: comment names sand"])

    def test_an_app_may_name_itself(self):
        self.assertEqual(self.problems({
            "launcher/main/apps/sand/sand.c": "/* sand steps here */\nint x;\n"}), [])

    def test_an_engine_document_line_naming_an_app_fails(self):
        found = self.problems({"docs/notes/Panel.md": "# Panel\n\nSand fell to 11 fps.\n"})
        self.assertEqual(found, ["docs/notes/Panel.md:3: document names sand"])

    def test_prose_spellings_of_an_underscored_name_all_match(self):
        found = self.problems({"docs/Gfx.md": "Render Lab\nrender-lab.png\n`render_lab`\nrenderlab\n"})
        self.assertEqual(len(found), 4)
        self.assertTrue(all(f.endswith("names render_lab") for f in found))

    def test_a_url_encoded_app_name_fails(self):
        found = self.problems({"docs/Gfx.md": "[tools](../render%5Flab/README.md)\n"})
        self.assertEqual(found, ["docs/Gfx.md:1: document names render_lab"])

    def test_an_apps_own_docs_folder_and_plans_may_name_it(self):
        self.assertEqual(self.problems({
            "docs/sand/Simulation.md": "Sand and render_lab.\n",
            "docs/plans/Roadmap.md": "Sand becomes an instance.\n"}), [])

    def test_a_markdown_file_inside_a_lower_layer_is_checked(self):
        found = self.problems({"launcher/test/stubs/README.md": "Stubs for app_sand.c.\n"})
        self.assertEqual(found, ["launcher/test/stubs/README.md:1: document names sand"])

    def test_the_diagnostics_build_variant_is_not_the_app(self):
        self.assertEqual(self.problems({"docs/Build.md":
            "The diagnostics build, the diagnostics variant, a diagnostics image,\n"
            "and build-diagnostics.yml.\n"}), [])
        found = self.problems({"docs/Build.md": "The Diagnostics app toggles it.\n"})
        self.assertEqual(found, ["docs/Build.md:1: document names diagnostics"])

    def test_a_name_wrapped_across_lines_is_still_a_name(self):
        found = self.problems({"docs/Gfx.md": "drawn by Render\nLab today\n",
                               "docs/List.md": "- drawn by Render\n  Lab today\n"})
        self.assertEqual(found, ["docs/Gfx.md:1: document names render_lab",
                                 "docs/List.md:1: document names render_lab"])

    def test_the_diagnostics_build_wrapped_across_lines_is_still_the_variant(self):
        self.assertEqual(self.problems({"docs/Build.md": "the diagnostics\nbuild links it\n"}), [])

    def test_a_longer_word_containing_a_name_is_not_a_name(self):
        self.assertEqual(self.problems({"docs/Gfx.md": "sandbox, thousand, sandstone\n"}), [])


if __name__ == "__main__":
    unittest.main()
