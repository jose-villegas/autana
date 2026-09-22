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

    def rule_ids(self, findings):
        return sorted(f.rule for f in findings)

    # ---- TRACKER-REF ------------------------------------------------

    def test_a_bd_issue_id_in_a_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* see bd autana-9xp for the follow-up */\nvoid gfx_init(void) {}\n")
            self.commit(root, "launcher")
            findings = check_style_audit.rule_tracker_and_commit_refs(root)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].rule, "TRACKER-REF")

    def test_a_full_commit_hash_in_a_doc_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "Pinned to commit 8275e0af7c16aa40c54ea2b90b7af83b1fe4eb4c.\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_tracker_and_commit_refs(root)
        self.assertEqual(len(findings), 1)

    def test_an_ordinary_compound_word_and_a_short_build_id_are_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "Run `autana screenshot` or `autana monitor`; build a195574e7177-diag.\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_tracker_and_commit_refs(root)
        self.assertEqual(findings, [])

    # ---- CALENDAR-DATE ------------------------------------------------

    def test_a_date_in_a_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* measured on device, 2026-09-13 */\nvoid gfx_init(void) {}\n")
            self.commit(root, "launcher")
            findings = check_style_audit.rule_calendar_dates(root)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].rule, "CALENDAR-DATE")

    def test_a_date_in_a_dated_log_txt_file_is_not_scanned(self):
        # docs/doc_review_ledger.txt is a real dated log in this tree - a
        # path\tdate\tnote row per review. It is a .txt, outside this rule's
        # .md/.c/.h scope, which is the exception the rule relies on.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/doc_review_ledger.txt", "docs/Guide.md\t2026-09-17\tchecked\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_calendar_dates(root)
        self.assertEqual(findings, [])

    def test_prose_with_no_date_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "Measured once on device, still true.\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_calendar_dates(root)
        self.assertEqual(findings, [])

    # ---- LIVING-DOC ------------------------------------------------

    def test_living_document_phrase_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "This is a living document.\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_living_document(root)
        self.assertEqual(len(findings), 1)

    def test_ordinary_prose_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "This document explains the build variants.\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_living_document(root)
        self.assertEqual(findings, [])

    # ---- INCLUDE-LAYER ------------------------------------------------

    def gfx_fixture(self, root):
        self.write(root, "launcher/main/gfx/gfx.h", "#pragma once\n")

    def test_a_relative_include_of_a_layer_header_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.gfx_fixture(root)
            self.write(root, "launcher/main/apps/sand/app_sand.c",
                      '#include "../../gfx/gfx.h"\n')
            self.commit(root, "launcher")
            findings = check_style_audit.rule_include_layering(root)
        self.assertEqual(len(findings), 1)
        self.assertIn('"gfx/gfx.h"', findings[0].message)

    def test_a_bare_layer_header_include_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.gfx_fixture(root)
            self.write(root, "launcher/main/apps/sand/app_sand.c", '#include "gfx.h"\n')
            self.commit(root, "launcher")
            findings = check_style_audit.rule_include_layering(root)
        self.assertEqual(len(findings), 1)

    def test_a_layer_qualified_include_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.gfx_fixture(root)
            self.write(root, "launcher/main/apps/sand/app_sand.c", '#include "gfx/gfx.h"\n')
            self.commit(root, "launcher")
            findings = check_style_audit.rule_include_layering(root)
        self.assertEqual(findings, [])

    def test_an_apps_internal_relative_include_is_not_flagged(self):
        # An app's own cross-reference to a sibling file in its own folder
        # is not what the layering rule is about - apps organise themselves.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/apps/sand/material.h", "#pragma once\n")
            self.write(root, "launcher/main/apps/sand/ui/brush_screen.c",
                      '#include "../material.h"\n')
            self.commit(root, "launcher")
            findings = check_style_audit.rule_include_layering(root)
        self.assertEqual(findings, [])

    # ---- PERSONAL-PATH ------------------------------------------------

    def test_a_windows_personal_path_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/device/device.py",
                      'IDF_PYTHON = r"C:\\Users\\jdoe\\.espressif\\python_env\\python.exe"\n')
            self.commit(root, "scripts")
            findings = check_style_audit.rule_personal_paths(root)
        self.assertEqual(len(findings), 1)

    def test_a_portable_home_relative_path_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/device/device.py",
                      'IDF_PYTHON_GLOB = "~/.espressif/python_env/idf*_env/Scripts/python.exe"\n')
            self.commit(root, "scripts")
            findings = check_style_audit.rule_personal_paths(root)
        self.assertEqual(findings, [])

    # ---- ANCHOR-LINK ------------------------------------------------

    def test_a_link_to_a_missing_heading_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "# Guide\n\n[gone](#nothing-here)\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_anchor_links(root)
        self.assertEqual(len(findings), 1)

    def test_an_em_dash_heading_slug_uses_a_double_hyphen(self):
        # GitHub's slugger drops the em dash but keeps both spaces around it
        # as separate hyphens, rather than collapsing them into one.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "### SD card \u2014 fully independent\n\n"
                      "[SD card](#sd-card--fully-independent)\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_anchor_links(root)
        self.assertEqual(findings, [])

    def test_a_link_to_a_real_heading_in_another_file_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/A.md", "[B](B.md#the-section)\n")
            self.write(root, "docs/B.md", "## The section\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_anchor_links(root)
        self.assertEqual(findings, [])

    # ---- STRAY-HTML-COMMENT --------------------------------------------

    def test_a_stray_html_comment_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "text\n<!-- TODO: remove this -->\nmore text\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_stray_html_comments(root)
        self.assertEqual(len(findings), 1)

    def test_a_known_escape_marker_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "C6 is historical. <!-- doc-vocabulary: ignore -->\n"
                      "<!-- BEGIN GENERATED -->\ntable\n<!-- END GENERATED -->\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_stray_html_comments(root)
        self.assertEqual(findings, [])

    # ---- DOC-CITATION / DOC-SECTION ------------------------------------

    def test_a_missing_doc_path_citation_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "See `docs/Gone.md` for details.\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_doc_citations(root)
        self.assertEqual([f.rule for f in findings], ["DOC-CITATION"])

    def test_a_quoted_section_matching_no_heading_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Target.md", "## Two cores: real heading\n")
            self.write(root, "docs/Guide.md", 'See Target.md\'s "Nothing like this" section.\n')
            self.commit(root, "docs")
            findings = check_style_audit.rule_doc_citations(root)
        self.assertEqual([f.rule for f in findings], ["DOC-SECTION"])

    def test_a_quoted_section_that_is_a_prefix_of_the_real_heading_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Target.md",
                      "## Two cores: chunk-parallel passes, and what stays serial\n")
            self.write(root, "docs/Guide.md", 'See Target.md\'s "Two cores" section.\n')
            self.commit(root, "docs")
            findings = check_style_audit.rule_doc_citations(root)
        self.assertEqual(findings, [])

    def test_an_existing_doc_path_citation_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Real.md", "# Real\n")
            self.write(root, "docs/Guide.md", "See `docs/Real.md`.\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_doc_citations(root)
        self.assertEqual(findings, [])

    # ---- ACCIDENTAL-BULLET ------------------------------------------------

    def test_a_bullet_right_after_unindented_prose_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "This sentence continues onto the next line where it\n"
                      "- 3 happens to start with a dash and a space\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_accidental_bullets(root)
        self.assertEqual(len(findings), 1)

    def test_a_bullet_after_an_indented_list_continuation_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "- **First item.** Some wrapped continuation text\n"
                      "  that is indented under the item above.\n"
                      "- Second item.\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_accidental_bullets(root)
        self.assertEqual(findings, [])

    def test_a_real_list_introduced_by_a_colon_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "The rules are:\n- one\n- two\n")
            self.commit(root, "docs")
            findings = check_style_audit.rule_accidental_bullets(root)
        self.assertEqual(findings, [])

    # ---- HEADING-COMMENT ------------------------------------------------

    def test_a_title_case_comment_inside_a_function_body_is_flagged(self):
        # A leading file-header comment keeps scan()'s "first comment in the
        # file is the banner" rule from mistaking the mid-function one for
        # it - the fixture needs both to isolate what this rule judges.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* gfx - fixture header. */\n\n"
                      "void\nfoo(void) {\n    int a = 1;\n    /* Setup */\n    a = 2;\n}\n")
            self.commit(root, "launcher")
            findings = check_style_audit.rule_heading_comments_in_functions(root)
        self.assertEqual(len(findings), 1)

    def test_the_same_text_as_a_file_level_divider_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* Setup */\n\nstatic int\nfoo(void) {\n    return 1;\n}\n")
            self.commit(root, "launcher")
            findings = check_style_audit.rule_heading_comments_in_functions(root)
        self.assertEqual(findings, [])

    def test_the_same_text_inside_an_array_initializer_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "static const int table[] = {\n    /* Values */\n    1, 2, 3,\n};\n")
            self.commit(root, "launcher")
            findings = check_style_audit.rule_heading_comments_in_functions(root)
        self.assertEqual(findings, [])

    def test_an_ordinary_sentence_comment_inside_a_function_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "void\nfoo(void) {\n    /* always safe here */\n    bar();\n}\n")
            self.commit(root, "launcher")
            findings = check_style_audit.rule_heading_comments_in_functions(root)
        self.assertEqual(findings, [])


class MainTest(unittest.TestCase):
    def test_exits_nonzero_and_prints_a_summary_line(self):
        import os
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
            subprocess.run(git + ["init", "-q"], cwd=root, check=True)
            (root / "docs").mkdir()
            (root / "docs" / "Guide.md").write_text("This is a living document.\n", encoding="utf-8")
            subprocess.run(git + ["add", "docs"], cwd=root, check=True)
            subprocess.run(git + ["commit", "-qm", "fixture"], cwd=root, check=True)
            cwd = os.getcwd()
            try:
                os.chdir(root)
                code = check_style_audit.main([])
            finally:
                os.chdir(cwd)
        self.assertEqual(code, 1)

    def test_a_clean_tree_exits_zero(self):
        import os
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
            subprocess.run(git + ["init", "-q"], cwd=root, check=True)
            (root / "docs").mkdir()
            (root / "docs" / "Guide.md").write_text("Nothing to see here.\n", encoding="utf-8")
            subprocess.run(git + ["add", "docs"], cwd=root, check=True)
            subprocess.run(git + ["commit", "-qm", "fixture"], cwd=root, check=True)
            cwd = os.getcwd()
            try:
                os.chdir(root)
                code = check_style_audit.main([])
            finally:
                os.chdir(cwd)
        self.assertEqual(code, 0)

    def test_rejects_arguments(self):
        self.assertEqual(check_style_audit.main(["--bogus"]), 2)


if __name__ == "__main__":
    unittest.main()
