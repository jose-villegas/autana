"""Regression tests for scripts/gates/check_style_audit.py."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import check_style_audit  # noqa: E402


class StyleAuditTest(unittest.TestCase):
    def write(self, root, path, text):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def commit(self, root, *paths):
        git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
        subprocess.run(git + ["init", "-q"], cwd=root, check=True)
        subprocess.run(git + ["add", *paths], cwd=root, check=True)
        subprocess.run(git + ["commit", "-qm", "fixture"], cwd=root, check=True)

    def rule_hits(self, root, rule_id, file_filter=None):
        findings, _ = check_style_audit.run_audit(root, rule_filter=rule_id, file_filter=file_filter)
        return findings

    def test_a_bd_issue_id_in_a_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* gfx - fixture header. */\n\n"
                      "void\ngfx_init(void) {\n"
                      "    /* see bd autana-9xp for the follow-up */\n}\n")
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "TRACKER-REF")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].severity, "ERROR")

    def test_a_bare_digit_led_tracker_id_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "See autana-9xp for the follow-up.\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "TRACKER-REF")
        self.assertEqual(len(findings), 1)

    def test_a_full_commit_hash_in_a_doc_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "Pinned to commit 8275e0af7c16aa40c54ea2b90b7af83b1fe4eb4c.\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "TRACKER-REF")
        self.assertEqual(len(findings), 1)

    def test_real_compound_words_and_a_short_build_id_are_not_flagged(self):
        # autana-cli, autana-screenshot, autana-monitor and autana-device
        # are all real, tracked, letter-only compounds in this repo - none
        # of them may start looking like a tracker id.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "Run `autana screenshot` (autana-screenshot), `autana monitor` "
                      "(autana-monitor), autana-cli, or check autana-device; "
                      "build a195574e7177-diag.\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "TRACKER-REF")
        self.assertEqual(findings, [])

    def test_a_date_in_a_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* gfx - fixture header. */\n\n"
                      "void\ngfx_init(void) {\n"
                      "    /* measured on device, 2026-09-13 */\n}\n")
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "CALENDAR-DATE")
        self.assertEqual(len(findings), 1)

    def test_a_date_in_a_dated_log_txt_file_is_not_scanned(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/doc_review_ledger.txt", "docs/Guide.md\t2026-09-17\tchecked\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "CALENDAR-DATE")
        self.assertEqual(findings, [])

    def test_living_document_phrase_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "This is a living document.\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "LIVING-DOC")
        self.assertEqual(len(findings), 1)

    def gfx_fixture(self, root):
        self.write(root, "launcher/main/gfx/gfx.h", "#pragma once\n")

    def test_a_relative_include_of_a_layer_header_is_flagged_and_fixed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.gfx_fixture(root)
            target = root / "launcher/main/apps/sand/app_sand.c"
            self.write(root, "launcher/main/apps/sand/app_sand.c", '#include "../../gfx/gfx.h"\n')
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-LAYER")
            self.assertEqual(len(findings), 1)
            self.assertTrue(findings[0].fixable)
            check_style_audit.run_fix(root, findings)
            self.assertEqual(target.read_text(encoding="utf-8"), '#include "gfx/gfx.h"\n')

    def test_a_bare_ambiguous_layer_header_is_reported_unfixable(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/shared.h", "#pragma once\n")
            self.write(root, "launcher/main/ui/shared.h", "#pragma once\n")
            self.write(root, "launcher/main/apps/sand/app_sand.c", '#include "shared.h"\n')
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-LAYER")
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0].fixable)

    def test_an_apps_internal_relative_include_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/apps/sand/material.h", "#pragma once\n")
            self.write(root, "launcher/main/apps/sand/ui/brush_screen.c",
                      '#include "../material.h"\n')
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-LAYER")
        self.assertEqual(findings, [])

    def layer_tree(self, root):
        # The 9 real layer folders, minimally populated, so
        # rule_include_direction's own LAYER_TIER-vs-tree check passes.
        for layer in check_style_audit.LAYER_TIER:
            self.write(root, f"launcher/main/{layer}/.keep", "")

    def test_a_same_tier_cross_folder_include_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            self.write(root, "launcher/main/gfx/gfx.c", '#include "render/r3d_project.h"\n')
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(len(findings), 1)
        self.assertIn("tier", findings[0].message)

    def test_including_a_strictly_lower_tier_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            self.write(root, "launcher/main/gfx/gfx.c", '#include "util/tune.h"\n')
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(findings, [])

    def test_a_same_folder_include_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            self.write(root, "launcher/main/gfx/gfx.c", '#include "gfx/gfx_dirty.h"\n')
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(findings, [])

    def test_a_documented_exception_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.layer_tree(root)
            self.write(root, "launcher/main/util/device_state.c", '#include "display/display.h"\n')
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "INCLUDE-DIRECTION")
        self.assertEqual(findings, [])

    def test_a_windows_personal_path_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/device/device.py",
                      'IDF_PYTHON = r"C:\\Users\\jdoe\\.espressif\\python_env\\python.exe"\n')
            self.commit(root, "scripts")
            findings = self.rule_hits(root, "PERSONAL-PATH")
        self.assertEqual(len(findings), 1)

    def test_a_portable_home_relative_path_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/device/device.py",
                      'GLOB = "~/.espressif/python_env/idf*_env/Scripts/python.exe"\n')
            self.commit(root, "scripts")
            findings = self.rule_hits(root, "PERSONAL-PATH")
        self.assertEqual(findings, [])

    def test_a_stray_html_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "text\n<!-- TODO: remove this -->\nmore text\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "STRAY-HTML-COMMENT")
        self.assertEqual(len(findings), 1)

    def test_a_multi_line_html_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "text\n<!-- a stray aside\nspanning two lines -->\nmore\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "STRAY-HTML-COMMENT")
        self.assertEqual(len(findings), 1)

    def test_a_known_escape_marker_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "C6 is historical. <!-- doc-vocabulary: ignore -->\n"
                      "<!-- BEGIN GENERATED -->\ntable\n<!-- END GENERATED -->\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "STRAY-HTML-COMMENT")
        self.assertEqual(findings, [])

    def test_a_bullet_right_after_unindented_prose_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "This sentence continues onto the next line where it\n"
                      "- 3 happens to start with a dash and a space\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "ACCIDENTAL-BULLET")
        self.assertEqual(len(findings), 1)

    def test_a_bullet_after_an_indented_list_continuation_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "- **First item.** Some wrapped continuation text\n"
                      "  that is indented under the item above.\n"
                      "- Second item.\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "ACCIDENTAL-BULLET")
        self.assertEqual(findings, [])

    def test_a_lazily_continued_list_item_is_not_flagged(self):
        # CommonMark's lazy continuation lets a list item's paragraph carry
        # on with no indentation at all - the paragraph this "- " sits in
        # started with its own list marker, so it is a sibling item, not an
        # accident.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "- First item text\n"
                      "continues here with no indentation at all\n"
                      "- Second item\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "ACCIDENTAL-BULLET")
        self.assertEqual(findings, [])

    def test_a_list_right_after_a_closing_fence_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "```sh\nexample\n```\n- a real list item\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "ACCIDENTAL-BULLET")
        self.assertEqual(findings, [])

    def test_a_real_list_introduced_by_a_colon_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "The rules are:\n- one\n- two\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "ACCIDENTAL-BULLET")
        self.assertEqual(findings, [])

    def test_consecutive_blank_lines_are_flagged_and_fixed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            target = root / "docs/Guide.md"
            self.write(root, "docs/Guide.md", "one\n\n\ntwo\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "BLANK-LINES")
            self.assertEqual(len(findings), 1)
            check_style_audit.run_fix(root, findings)
            self.assertEqual(target.read_text(encoding="utf-8"), "one\n\ntwo\n")

    def test_a_generated_tables_own_blank_lines_are_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Table.md",
                      "<!-- BEGIN GENERATED -->\na\n\n\nb\n<!-- END GENERATED -->\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "BLANK-LINES")
        self.assertEqual(findings, [])

    def test_trailing_blank_lines_are_flagged_and_fixed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            target = root / "docs/Guide.md"
            self.write(root, "docs/Guide.md", "one\ntwo\n\n\n")
            self.commit(root, "docs")
            findings = self.rule_hits(root, "TRAILING-BLANK-LINES")
            self.assertEqual(len(findings), 1)
            check_style_audit.run_fix(root, findings)
            self.assertEqual(target.read_text(encoding="utf-8"), "one\ntwo\n")

    def test_a_drawn_rule_comment_is_flagged_and_fixed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            target = root / "launcher/main/gfx/gfx.c"
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* gfx - fixture header. */\n\n"
                      "/* ==== Setup ==== */\nvoid\ngfx_init(void) {}\n")
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "DRAWN-COMMENT-RULE")
            self.assertEqual(len(findings), 1)
            check_style_audit.run_fix(root, findings)
            self.assertIn("/* Setup */", target.read_text(encoding="utf-8"))
            self.assertNotIn("====", target.read_text(encoding="utf-8"))

    def test_a_title_case_comment_inside_a_function_body_is_a_warning(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* gfx - fixture header. */\n\n"
                      "void\nfoo(void) {\n    int a = 1;\n    /* Setup */\n    a = 2;\n}\n")
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "HEADING-COMMENT")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].severity, "WARN")

    def test_the_same_text_as_a_file_level_divider_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* Setup */\n\nstatic int\nfoo(void) {\n    return 1;\n}\n")
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "HEADING-COMMENT")
        self.assertEqual(findings, [])

    def test_the_same_text_inside_an_array_initializer_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "static const int table[] = {\n    /* Values */\n    1, 2, 3,\n};\n")
            self.commit(root, "launcher")
            findings = self.rule_hits(root, "HEADING-COMMENT")
        self.assertEqual(findings, [])


class MainTest(unittest.TestCase):
    def write(self, root, path, text):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def commit(self, root, *paths):
        git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
        subprocess.run(git + ["init", "-q"], cwd=root, check=True)
        subprocess.run(git + ["add", *paths], cwd=root, check=True)
        subprocess.run(git + ["commit", "-qm", "fixture"], cwd=root, check=True)

    def run_main(self, root, argv):
        import os
        cwd = os.getcwd()
        try:
            os.chdir(root)
            return check_style_audit.main(argv)
        finally:
            os.chdir(cwd)

    def test_a_clean_tree_exits_zero(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "Nothing to see here.\n")
            self.commit(root, "docs")
            code = self.run_main(root, [])
        self.assertEqual(code, 0)

    def test_an_error_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "This is a living document.\n")
            self.commit(root, "docs")
            code = self.run_main(root, [])
        self.assertEqual(code, 1)

    def test_a_warning_alone_passes_without_strict_and_fails_with_it(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* gfx - fixture header. */\n\n"
                      "void\nfoo(void) {\n    /* Setup */\n    bar();\n}\n")
            self.commit(root, "launcher")
            plain = self.run_main(root, [])
            strict = self.run_main(root, ["--strict"])
        self.assertEqual(plain, 0)
        self.assertEqual(strict, 1)

    def test_fix_rewrites_the_file_and_the_rerun_is_clean(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            target = root / "docs/Guide.md"
            self.write(root, "docs/Guide.md", "one\n\n\ntwo\n")
            self.commit(root, "docs")
            code = self.run_main(root, ["--fix"])
            self.assertEqual(code, 0)
            self.assertEqual(target.read_text(encoding="utf-8"), "one\n\ntwo\n")

    def test_a_file_filter_matching_nothing_exits_two(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "text\n")
            self.commit(root, "docs")
            code = self.run_main(root, ["--file", "zzz-nonexistent-zzz"])
        self.assertEqual(code, 2)

    def test_an_unknown_rule_exits_two(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "text\n")
            self.commit(root, "docs")
            code = self.run_main(root, ["--rule", "NOT-A-REAL-RULE"])
        self.assertEqual(code, 2)

    def test_rejects_unknown_arguments(self):
        self.assertEqual(check_style_audit.main(["--bogus"]), 2)


if __name__ == "__main__":
    unittest.main()
