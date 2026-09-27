"""Regression tests for scripts/gates/check_comment_symbols.py."""
import pathlib
import sys
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_comment_symbols  # noqa: E402
import idf_vocabulary  # noqa: E402
from fake_idf import fake_idf, fake_toolchain  # noqa: E402

# An ESP-IDF that defines none of the names a test cites.
NO_OUTSIDE_NAMES = idf_vocabulary.OutsideVocabulary(None)


class ProblemsTest(unittest.TestCase):
    """One tree per case: `files` maps a path under launcher/ to its text."""

    def problems(self, files, outside=NO_OUTSIDE_NAMES, unchecked=None):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp) / "launcher"
            for path, text in files.items():
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")
            if callable(outside):
                outside = outside(pathlib.Path(temp))
            found = check_comment_symbols.problems(str(root), outside, unchecked)
            return [line.split(": ", 1)[1] for line in found]

    def test_a_name_only_esp_idf_defines_resolves_and_one_in_neither_fails(self):
        def outside(base):
            return idf_vocabulary.outside_vocabulary(
                fake_idf(base / "esp-idf"), fake_toolchain(base / "espressif"), base / "cache")
        found = self.problems({
            "main/a.c": "/* fake_ll_cal_clock() and fake_hypot() set it, not ghost_ll_function() */\n",
        }, outside)
        self.assertEqual(found, ["comment names ghost_ll_function(), which does not exist"])

    def test_without_esp_idf_a_name_this_tree_lacks_is_counted_not_failed(self):
        unchecked = []
        found = self.problems({
            "main/a.c": "#define SAND_REAL 1\n"
                        "/* fake_ll_cal_clock() sets it; SAND_GONE bounds it */\n"
                        "int f(void) { return SAND_REAL; }\n",
        }, None, unchecked)
        self.assertEqual(found, ["comment names SAND_GONE, which does not exist"])
        self.assertEqual([line.split(": ", 1)[1] for line in unchecked], ["fake_ll_cal_clock()"])

    def test_a_constant_only_another_comment_spells_is_reported(self):
        # CONDUCT_REACH-style citations were never checked: the vocabulary
        # took every capitalised word in the file, comments included, so a
        # dead macro one comment still mentioned vouched for every other.
        found = self.problems({
            "main/a.c": "#define SAND_REAL 1\n"
                        "/* SAND_GONE used to live here */\n"
                        "/* see SAND_GONE */\n"
                        "int f(void) { return SAND_REAL; }\n",
        })
        self.assertEqual(found, ["comment names SAND_GONE, which does not exist"] * 2)

    def test_a_constant_outside_every_project_family_is_the_sdks(self):
        found = self.problems({
            "main/a.c": "#define SAND_REAL 1\n/* returns ESP_FAIL on a bad pin */\n",
        })
        self.assertEqual(found, [])

    def test_a_token_the_code_prints_is_real(self):
        found = self.problems({
            "main/a.c": "#define TUNE_BUF 8\n"
                        "/* the host waits for TUNE_OK */\n"
                        "void f(void) { printf(\"\\nTUNE_OK\\n\"); }\n",
        })
        self.assertEqual(found, [])

    def test_a_short_function_name_is_checked_too(self):
        # Names under five characters were skipped on both sides, so a
        # comment citing lit() was never checked, and a real lit() never
        # counted as defined.
        found = self.problems({
            "main/a.c": "/* see lit() and gone() */\nstatic int lit(void) { return 0; }\n",
        })
        self.assertEqual(found, ["comment names gone(), which does not exist"])

    def test_a_name_split_across_a_line_break_is_reported(self):
        found = self.problems({
            "main/a.c": "/* both map through canvas_physical_\n"
                        " * rect() */\n"
                        "static void canvas_physical_rect(void) {}\n",
        })
        self.assertEqual(found, ["comment names rect(), which does not exist"])

    def test_a_script_function_counts_only_when_the_comment_names_the_script(self):
        found = self.problems({
            "tools/gen.py": "def count_runs(rows):\n    return 0\n",
            # Code between the two, or the scanner reads them as one comment.
            "main/a.c": "/* checked against gen.py's count_runs() */\n"
                        "int a;\n"
                        "/* checked against count_runs() */\n",
        })
        self.assertEqual(found, ["comment names count_runs(), which is defined only by a script "
                                 "the comment does not name"])

    def test_a_suffix_and_a_method_are_not_citations(self):
        found = self.problems({
            "main/a.c": "/* ui_scroll_begin()/_end() wrap it; json.loads() reads it */\n"
                        "void ui_scroll_begin(void) {}\n",
        })
        self.assertEqual(found, [])

    def test_a_name_pinned_to_the_wrong_file_is_reported(self):
        # material_colours()'s comment was cited as living in material.c
        # after the function had moved to material_palette.c.
        found = self.problems({
            "main/material.c": "int material_init(void) { return 0; }\n",
            "main/material_palette.c": "int material_colours(void) { return 0; }\n",
            "main/a.c": "/* material_colours() (material.c) picks it */\n"
                        "/* material_colours() (material_palette.c) picks it */\n"
                        "/* material_colours()'s own comment, material.c) */\n"
                        "/* material_colours() (gone.c) picks it */\n",
        })
        self.assertEqual(found, [
            "comment places material_colours in material.c, whose code never names it",
            "comment places material_colours in material.c, whose code never names it",
            "comment places material_colours in gone.c, which does not exist",
        ])

    def test_a_constant_pinned_to_its_header_resolves(self):
        found = self.problems({
            "main/sand_impulse.h": "#define SAND_IMPULSE_CAP 4\n",
            "main/a.c": "/* bounded (SAND_IMPULSE_CAP, sand_impulse.h) */\n"
                        "/* bounded (SAND_IMPULSE_CAP, sand.h) */\n",
            "main/sand.h": "int sand_step(void);\n",
        })
        self.assertEqual(found, ["comment places SAND_IMPULSE_CAP in sand.h, whose code never names it"])

    def test_a_cited_heading_no_other_comment_has_is_reported(self):
        # suite_sand_liquid_depth.c cited paint_row_n()'s "THE PROJECTION",
        # a heading no comment anywhere carried.
        found = self.problems({
            "main/a.h": "/* WHY DERIVED, NOT FIXED: the count follows the width. */\n",
            "main/b.c": "/* see a.h's \"WHY DERIVED, NOT FIXED\" */\n"
                        "/* see paint_row_n()'s own \"THE PROJECTION\" */\n"
                        "void paint_row_n(void) {}\n",
        })
        self.assertEqual(found, ['comment cites "THE PROJECTION", which no other comment has'])

    def test_configuration_options_resolve_from_kconfig_and_sdkconfig(self):
        found = self.problems({
            "main/Kconfig.projbuild": "config LAUNCHER_DEVELOPMENT\n    bool\n",
            "sdkconfig.defaults": "CONFIG_LAUNCHER_SELFTEST=y\n",
            "main/a.c": "/* CONFIG_LAUNCHER_DEVELOPMENT and CONFIG_LAUNCHER_SELFTEST gate it,\n"
                        " * not CONFIG_LAUNCHER_GONE */\n",
        })
        self.assertEqual(found, ["comment names CONFIG_LAUNCHER_GONE, which does not exist"])


if __name__ == "__main__":
    unittest.main()
