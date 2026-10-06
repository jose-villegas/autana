"""Regression tests for documentation citation tools."""
import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
import gate_tree
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_doc_citations  # noqa: E402
import check_doc_constants  # noqa: E402
import check_doc_index  # noqa: E402
import check_doc_vocabulary  # noqa: E402
import doc_citers  # noqa: E402
import doc_drift  # noqa: E402
import idf_vocabulary  # noqa: E402
from fake_idf import fake_idf, fake_toolchain  # noqa: E402

# An ESP-IDF that defines none of the names a test cites.
NO_OUTSIDE_NAMES = idf_vocabulary.OutsideVocabulary(None)


class OptionLinkTest(unittest.TestCase):
    def check(self, text):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            doc = root / "docs" / "Guide.md"
            doc.parent.mkdir(parents=True)
            doc.write_text(text, encoding="utf-8")
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "add", "docs"], cwd=root, check=True)
            return check_doc_index.check_option_links(root)

    def test_option_rows_require_their_own_unambiguous_heading(self):
        table = "| Option | Value | Option link |\n|---|---|---|\n"
        self.assertEqual(self.check(table + "| `alpha` | x | [alpha](#alpha) |\n\n### alpha\n"), [])
        self.assertEqual(self.check(table + "| `alpha` | x | [beta](#beta) |\n\n### alpha\n### beta\n"),
                         [("docs/Guide.md", 3, "`alpha`", "must link to #alpha")])
        self.assertEqual(self.check(table + "| `process` | x | [process](#process) |\n\n### Process\n### process\n"),
                         [("docs/Guide.md", 3, "`process`", "#process is shared by another heading")])
        self.assertEqual(self.check(table + "| `alpha` | [alpha](#alpha) |\n\n### alpha\n"),
                         [("docs/Guide.md", 3, "`alpha`", "wrong number of columns")])


class DocCitationTest(unittest.TestCase):
    def fixture(self, root):
        gate_tree.write(root, "launcher/main/example.c",
                   "void live_function(void) {}\n#define LIVE_MACRO 1\n")
        gate_tree.write(root, "scripts/live.py", "def helper():\n    pass\n")
        gate_tree.write(root, "docs/Guide.md", """`live_function()` `missing_function()`
`example.c` `missing.h` `LIVE_MACRO` `LIVE_MISSING`
```sh
missing_function() LIVE_MISSING missing.sh
```
""")

    def test_checker_reports_only_missing_repo_citations(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.fixture(root)
            missing = check_doc_citations.check(root, NO_OUTSIDE_NAMES)
        self.assertEqual(
            [(item.kind, item.value) for item in missing],
            [("function", "missing_function"), ("path", "missing.h"),
             ("macro", "LIVE_MISSING")],
        )

    def test_tokenizer_ignores_pasted_identifier_fragments(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/example.c", "void live_function(void) {}\n")
            gate_tree.write(root, "docs/Guide.md",
                       "`live_function()` `_count()` `palette##_set_lut16()` `<prefix>_count()`\n")
            found = list(check_doc_citations.citations(root))
        self.assertEqual([(item.kind, item.value) for item in found],
                         [("function", "live_function")])

    def test_constant_checker_matches_and_reports_mismatches(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/example.h", "#define LIVE_LIMIT 32\nenum { LIVE_ENUM = 9 };\n")
            gate_tree.write(root, "docs/Guide.md",
                       "`LIVE_LIMIT` is 32. `LIVE_ENUM` is 9.\n"
                       "`LIVE_LIMIT` is 16.\n| `LIVE_LIMIT` | 16 |\n")
            found = check_doc_constants.check(root)
        self.assertEqual([(item.line, item.name, item.claimed, item.defined) for item in found],
                         [(2, "LIVE_LIMIT", 16, 32), (3, "LIVE_LIMIT", 16, 32)])

    def test_constant_checker_reads_only_tracked_sources(self):
        # A build downloads third-party components into managed_components/.
        # One of them (a rapidjson header) declares `enum { value = 1 }`, which
        # paired with the prose "the honest value is 0" on any built checkout.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/example.h", "#define LIVE_LIMIT 32\n")
            gate_tree.write(root, "launcher/managed_components/lib/json.h", "enum { value = 1 };\n")
            gate_tree.write(root, "docs/Guide.md", "the honest value is 0, not a stale one.\n`LIVE_LIMIT` is 16.\n")
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
            subprocess.run(git + ["init", "-q"], cwd=root, check=True)
            subprocess.run(git + ["add", "launcher/main", "docs"], cwd=root, check=True)
            subprocess.run(git + ["commit", "-qm", "fixture"], cwd=root, check=True)
            found = check_doc_constants.check(root)
        self.assertEqual([(item.name, item.claimed, item.defined) for item in found],
                         [("LIVE_LIMIT", 16, 32)])

    def test_constant_checker_ignores_managed_components_outside_git(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/managed_components/lib/json.h", "enum { value = 1 };\n")
            gate_tree.write(root, "docs/Guide.md", "the honest value is 0, not a stale one.\n")
            found = check_doc_constants.check(root)
        self.assertEqual(found, [])

    def test_constant_checker_skips_ambiguous_historical_and_escaped_values(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/a.h", "#define AMBIGUOUS 4\n#define LIVE_LIMIT 32\n")
            gate_tree.write(root, "launcher/main/b.h", "#define AMBIGUOUS 8\n")
            gate_tree.write(root, "docs/Guide.md",
                       "`AMBIGUOUS` is 1.\n`LIVE_LIMIT` was 16.\n"
                       "`LIVE_LIMIT` is 8. <!-- doc-constants: ignore -->\n")
            found = check_doc_constants.check(root)
        self.assertEqual(found, [])

    def test_constant_checker_checks_current_claim_beside_a_hypothetical(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/example.h", "#define LIVE_LIMIT 32\n")
            gate_tree.write(root, "docs/Guide.md",
                       "`LIVE_LIMIT` is 16. At `LIVE_LIMIT` = 8 it would fail.\n")
            found, skipped = check_doc_constants.check(root, verbose=True)
        self.assertEqual([(item.name, item.claimed, item.defined) for item in found], [
            ("LIVE_LIMIT", 16, 32),
        ])
        self.assertIn(("docs/Guide.md", 1, "skipped-on-hypothetical"), skipped)

    def test_constant_checker_ignores_a_hypothetical_only_claim(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/example.h", "#define LIVE_LIMIT 32\n")
            gate_tree.write(root, "docs/Guide.md",
                       "At `LIVE_LIMIT` = 16 the system would fail.\n")
            found, skipped = check_doc_constants.check(root, verbose=True)
        self.assertEqual(found, [])
        self.assertIn(("docs/Guide.md", 1, "skipped-on-hypothetical"), skipped)

    def test_constant_checker_requires_an_explicit_constant_value_claim(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/example.h", "#define back 1\n#define LIVE_LIMIT 32\n")
            gate_tree.write(root, "docs/Guide.md", """\
back, the honest value is 0, not whatever a different part of the pool last left there.
`LIVE_LIMIT` = 16.
| Constant | value |
| --- | --- |
| `LIVE_LIMIT` | 16 |
""")
            found = check_doc_constants.check(root)
        self.assertEqual([(item.line, item.name, item.claimed, item.defined) for item in found], [
            (2, "LIVE_LIMIT", 16, 32),
            (3, "LIVE_LIMIT", 16, 32),
        ])

    def test_constant_checker_compares_matching_units_and_material_table_fields(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/example.h", "#define CONDUCT_REACH 32\n#define FRAME_MS 16\n")
            gate_tree.write(root, "launcher/main/apps/sand/material.c", """\
const material_t materials[] = {
    TWIN_ROW(MAT_GLASS, { .name = "Glass", .density = 121, }),
};
const reaction_t reactions[] = {
    [MAT_GLASS] = { .heat_chance = 8, },
};
static const char* const extended_names[] = {
    [MATX_METAL] = "Metal", [8] = "Gunpowder",
};
const reaction_t extended_reactions[] = {
    [MATX_METAL] = { .dissolvable = 1, },
#define GUNPOWDER_REACTION { .soaked_chance = 16, }
    [8] = GUNPOWDER_REACTION,
};
""")
            gate_tree.write(root, "docs/Guide.md", """\
`CONDUCT_REACH` (99 cells). `CONDUCT_REACH`, 98-cell wide. `CONDUCT_REACH` is a 97-cell run.
`FRAME_MS` is 99 cells. `FRAME_MS` is 99 ms.
| Material | density |
| --- | --- |
| Glass | 200 |
Glass heat_chance 16. `soaked_chance` 8.
```mermaid
Acid -->|"dissolvable 110"| Metal
```
""")
            found, skipped = check_doc_constants.check(root, verbose=True)
        self.assertEqual([(item.name, item.claimed, item.defined) for item in found], [
            ("CONDUCT_REACH", 99, 32),
            ("CONDUCT_REACH", 97, 32),
            ("FRAME_MS", 99, 16),
            ("Glass.density", 200, 121),
            ("Glass.heat_chance", 16, 8),
            ("Gunpowder.soaked_chance", 8, 16),
            ("Metal.dissolvable", 110, 1),
        ])
        self.assertIn(("docs/Guide.md", 2, "skipped-on-unit"), skipped)

    def test_reverse_index_reports_deleted_cited_function_as_json(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.com"],
                           cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=root,
                           check=True)
            gate_tree.write(root, "launcher/main/example.c", "void old_name(void) {}\n")
            gate_tree.write(root, "docs/Guide.md", "Use `old_name()`.\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=root, check=True)
            base = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                                  check=True, capture_output=True, text=True).stdout.strip()
            gate_tree.write(root, "launcher/main/example.c", "void new_name(void) {}\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "rename"], cwd=root, check=True)
            result = subprocess.run(
                [sys.executable, str(SCRIPTS / "docs_touched_by.py"), base, "--json"],
                cwd=root, check=True, capture_output=True, text=True)
        self.assertEqual(json.loads(result.stdout)["citations"], [{
            "doc": "docs/Guide.md", "kind": "function", "line": 1,
            "symbol": "old_name",
        }])

    def test_drift_uses_the_later_review_date(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=root, check=True)
            gate_tree.write(root, "docs/Guide.md", "Guide\n")
            gate_tree.write(root, "launcher/main/example.c", "void live_function(void) {}\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            committed = "2026-09-10T12:00:00+0000"
            subprocess.run(["git", "commit", "-qm", "base"], cwd=root, check=True,
                           env={**os.environ, "GIT_AUTHOR_DATE": committed,
                                "GIT_COMMITTER_DATE": committed})
            gate_tree.write(root, "docs/doc_review_ledger.txt", "docs/Guide.md\t2026-09-17\tchecked\n")
            rows = doc_drift.report(root, today=__import__("datetime").date(2026, 9, 18))
        guide = next(row for row in rows if row["doc"] == "docs/Guide.md")
        self.assertEqual(guide["age"], 1)

    def test_drift_ranks_changed_cited_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=root, check=True)
            gate_tree.write(root, "docs/Guide.md", "See `launcher/main/example.c`.\n")
            gate_tree.write(root, "launcher/main/example.c", "int example = 1;\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=root, check=True)
            gate_tree.write(root, "launcher/main/example.c", "int example = 2;\n")
            subprocess.run(["git", "commit", "-am", "change", "-q"], cwd=root, check=True)
            row = next(row for row in doc_drift.report(root) if row["doc"] == "docs/Guide.md")
        self.assertEqual((len(row["files"]), row["rank"] - row["age"]), (1, 30))

    def test_vocabulary_gate_honours_inline_and_previous_line_escapes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "scripts/gates/doc_vocabulary.txt", "C6\twrong board\n")
            gate_tree.write(root, "docs/Guide.md",
                       "C6 is historical. <!-- doc-vocabulary: ignore -->\n"
                       "<!-- doc-vocabulary: ignore -->\n"
                       "C6 was the prior target.\n")
            found = check_doc_vocabulary.check(root)
        self.assertEqual(found, [])

    def test_vocabulary_gate_reports_unescaped_retired_term(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "scripts/gates/doc_vocabulary.txt", "C6\twrong board\n")
            gate_tree.write(root, "docs/Guide.md", "C6 is not the current board.\n")
            found = check_doc_vocabulary.check(root)
        self.assertEqual([(path, term) for path, _, term, _ in found], [("docs/Guide.md", "C6")])

    def test_a_plan_may_cite_what_is_not_built_yet(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.fixture(root)
            gate_tree.write(root, "docs/plans/Future-Plan.md", "`planned_function()` `planned.h`\n")
            missing = check_doc_citations.check(root, NO_OUTSIDE_NAMES)
        self.assertNotIn("docs/plans/Future-Plan.md", [item.doc for item in missing])
        self.assertIn("docs/Guide.md", [item.doc for item in missing])

    def test_a_plans_named_file_outside_the_folder_is_checked(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/plans-archive.md", "`planned_function()`\n")
            missing = check_doc_citations.check(root, NO_OUTSIDE_NAMES)
        self.assertEqual([(item.doc, item.value) for item in missing],
                         [("docs/plans-archive.md", "planned_function")])

    def test_another_gates_marker_or_html_comment_does_not_silence_a_citation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md",
                       "`gone_a()` <!-- doc-vocabulary: ignore -->\n"
                       "`gone_b()` <!-- doc-constants: ignore -->\n"
                       "`gone_c()` <!-- gone_c() -->\n")
            missing = check_doc_citations.check(root, NO_OUTSIDE_NAMES)
        self.assertEqual([item.value for item in missing], ["gone_a", "gone_b", "gone_c"])

    def test_a_name_only_esp_idf_defines_resolves_and_one_in_neither_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            base = pathlib.Path(temp)
            root = base / "repo"
            gate_tree.write(root, "launcher/main/example.c", "void live_function(void) {}\n")
            gate_tree.write(root, "docs/Guide.md",
                       "`live_function()` `fake_ll_cal_clock()` in `hal/fake_ll.h`, `idf.py`,\n"
                       "`CONFIG_FAKE_LFN_NONE`, `FAKE_FREQ_DEFAULT`, `fake_hypot()` in `math.h`;\n"
                       "`ghost_ll_function()` `GHOST_FREQ` `hal/ghost_ll.h`\n")
            outside = idf_vocabulary.outside_vocabulary(
                fake_idf(base / "esp-idf"), fake_toolchain(base / "espressif"), base / "cache")
            missing = check_doc_citations.check(root, outside)
        self.assertEqual([(item.line, item.value) for item in missing],
                         [(3, "ghost_ll_function"), (3, "GHOST_FREQ"), (3, "hal/ghost_ll.h")])

    def run_main(self, root, *flags):
        output = io.StringIO()
        missing_idf = pathlib.Path(root) / "no-esp-idf"
        with mock.patch.dict(os.environ, {"IDF_PATH": str(missing_idf)}), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            code = check_doc_citations.main(["--root", str(root), *flags])
        return code, output.getvalue()

    def test_without_esp_idf_a_name_this_tree_lacks_is_counted_not_failed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/main/example.c", "void live_function(void) {}\n")
            gate_tree.write(root, "docs/Guide.md", "`live_function()` `fake_ll_cal_clock()`\n")
            code, output = self.run_main(root)
        self.assertEqual(code, 0)
        self.assertIn("1 cited name this tree does not define went unchecked: no ESP-IDF at", output)

    def test_without_esp_idf_a_section_citation_is_still_checked(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Other.md", "# Real\n")
            gate_tree.write(root, "docs/Guide.md", "See `Other.md`'s \"Gone\".\n")
            code, output = self.run_main(root)
        self.assertEqual(code, 1)
        self.assertIn('"Gone" is not a heading in Other.md', output)

    def test_require_idf_makes_a_missing_esp_idf_an_error(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "nothing cited\n")
            code, output = self.run_main(root, "--require-idf")
        self.assertEqual(code, 2)
        self.assertIn("no ESP-IDF at", output)

    def test_require_idf_fails_when_no_toolchain_c_library_is_found(self):
        with tempfile.TemporaryDirectory() as temp:
            base = pathlib.Path(temp)
            root = base / "repo"
            gate_tree.write(root, "docs/Guide.md", "nothing cited\n")
            idf = fake_idf(base / "esp-idf")
            (base / "no-tools").mkdir()
            output = io.StringIO()
            with mock.patch.dict(os.environ, {"IDF_PATH": str(idf), "IDF_TOOLS_PATH": str(base / "no-tools")}), \
                    mock.patch.object(idf_vocabulary, "cache_directory", return_value=base / "cache"), \
                    contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                code = check_doc_citations.main(["--root", str(root), "--require-idf"])
        self.assertEqual(code, 2)
        self.assertIn("C library", output.getvalue())

    def test_a_constant_an_mjs_script_reads_resolves(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "scripts/gates/check.mjs",
                       "// CHECK_COMMENTED_ONLY is only named here\n"
                       "const extra = process.env.CHECK_EXTRA_ARGS;\n")
            gate_tree.write(root, "docs/Guide.md", "`CHECK_EXTRA_ARGS` `CHECK_COMMENTED_ONLY`\n")
            missing = check_doc_citations.check(root, NO_OUTSIDE_NAMES)
        self.assertEqual([item.value for item in missing], ["CHECK_COMMENTED_ONLY"])

    def test_section_citation_after_the_doc_is_flagged_when_missing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Target.md", "## Two cores: real heading\n")
            gate_tree.write(root, "docs/Guide.md", 'See Target.md\'s "Nothing like this" section.\n')
            missing = check_doc_citations.unresolved_sections(root)
        self.assertEqual(len(missing), 1)
        self.assertEqual(missing[0][0].target_doc, "Target.md")

    def test_a_bare_doc_name_never_resolves_into_an_untracked_nested_checkout(self):
        # A checkout may hold untracked docs with the same names, which CI never
        # has. A citation must find the tracked doc.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/sand/Architecture.md", "## The grid, in one byte\n")
            gate_tree.write(root, "docs/Guide.md", 'See Architecture.md\'s "The grid, in one byte".\n')
            gate_tree.write(root, "notes/Architecture.md", "## Something else\n")
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
            subprocess.run(git + ["init", "-q"], cwd=root, check=True)
            subprocess.run(git + ["add", "docs"], cwd=root, check=True)
            subprocess.run(git + ["commit", "-qm", "fixture"], cwd=root, check=True)
            missing = check_doc_citations.unresolved_sections(root)
        self.assertEqual(missing, [])

    def test_section_citation_is_a_shorthand_prefix_of_the_real_heading(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Target.md",
                      "## Two cores: chunk-parallel passes, and what stays serial\n")
            gate_tree.write(root, "docs/Guide.md", 'See Target.md\'s "Two cores" section.\n')
            missing = check_doc_citations.unresolved_sections(root)
        self.assertEqual(missing, [])

    def test_section_citation_before_the_doc_with_and_is_resolved(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Target.md", "## Partial updates\n## Still untapped\n")
            gate_tree.write(root, "docs/Guide.md",
                      'See "Partial updates" and "Still untapped" in Target.md for more.\n')
            missing = check_doc_citations.unresolved_sections(root)
        self.assertEqual(missing, [])

    def test_section_citation_with_a_backtick_around_the_doc_name_is_read(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Target.md", "## Making room\n")
            gate_tree.write(root, "docs/Guide.md", '`Target.md`\'s "Making room" is the spot.\n')
            missing = check_doc_citations.unresolved_sections(root)
        self.assertEqual(missing, [])

    def test_section_citation_in_a_c_comment_is_checked(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Target.md", "## Real section\n")
            gate_tree.write(root, "launcher/main/example.h",
                      '/* see docs/Target.md\'s "Not real" for background. */\n')
            missing = check_doc_citations.unresolved_sections(root)
        self.assertEqual(len(missing), 1)
        self.assertEqual(missing[0][0].doc, "launcher/main/example.h")

    def test_section_citation_wrapped_across_a_comment_line_is_still_read(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Target.md", "## Still untapped\n")
            gate_tree.write(root, "launcher/main/example.h",
                      '/* See docs/Target.md\'s "Partial updates" and "Still\n'
                      ' * untapped" for the full reasoning. */\n')
            missing = check_doc_citations.unresolved_sections(root)
        self.assertEqual(len(missing), 1)
        self.assertEqual(missing[0][0].section, "Partial updates")

    def test_anchor_link_matching_no_heading_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "# Guide\n\n[gone](#nothing-here)\n")
            bad = check_doc_index.check_anchors(root)
        self.assertEqual(len(bad), 1)

    def test_anchor_link_to_a_real_heading_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/A.md", "[B](B.md#the-section)\n")
            gate_tree.write(root, "docs/B.md", "## The section\n")
            bad = check_doc_index.check_anchors(root)
        self.assertEqual(bad, [])

    def test_anchor_link_from_a_tool_readme_into_docs_is_checked(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md", "# Guide\n")
            gate_tree.write(root, "launcher/tools/README.md", "[gone](../../docs/Guide.md#gone)\n")
            bad = check_doc_index.check_anchors(root)
        self.assertEqual(len(bad), 1)

    def test_anchor_link_slug_uses_a_double_hyphen_for_an_em_dash(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/Guide.md",
                      "### SD card — fully independent\n\n"
                      "[SD card](#sd-card--fully-independent)\n")
            bad = check_doc_index.check_anchors(root)
        self.assertEqual(bad, [])

    def test_anchor_link_to_a_non_markdown_target_is_not_checked(self):
        # `#L3` in a link to a script is a line number, not a heading, and
        # a Python "# comment" in that file is not a Markdown heading either.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "scripts/a.py", "# comment\nprint(1)\n")
            gate_tree.write(root, "docs/Guide.md", "[a.py](../scripts/a.py#L3)\n")
            bad = check_doc_index.check_anchors(root)
        self.assertEqual(bad, [])

    def test_anchor_link_to_a_directory_is_not_checked(self):
        # A directory link ([notes](docs/notes/#top)) resolves to a folder,
        # not a .md file; it is what reachable() expands to that folder's
        # README.md, a step check_anchors() does not need to repeat.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "docs/notes/README.md", "# Notes\n")
            gate_tree.write(root, "docs/Guide.md", "[notes](notes/#top)\n")
            bad = check_doc_index.check_anchors(root)
        self.assertEqual(bad, [])

    def test_index_reports_documents_no_link_reaches(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "README.md", "[notes](docs/notes/)\n`docs/Named.md`\n")
            gate_tree.write(root, "docs/notes/README.md", "[Board](Board.md)\n")
            gate_tree.write(root, "docs/notes/Board.md", "[up](../Linked.md#top)\n")
            gate_tree.write(root, "docs/Linked.md", "text\n")
            gate_tree.write(root, "docs/Named.md", "only named in backticks\n")
            gate_tree.write(root, "docs/Orphan.md", "nothing links here\n")
            orphans = check_doc_index.check(root)
        self.assertEqual(orphans, ["docs/Named.md", "docs/Orphan.md"])

    def citers_repo(self, root):
        gate_tree.write(root, "launcher/main/liquid.c",
                   "static int\nfind_level(int x) {\n    return x;\n}\n\n"
                   "static int\nspread(int x) {\n    return x + 1;\n}\n")
        gate_tree.write(root, "docs/Liquid.md", "`find_level()` picks the level; see `liquid.c`.\n")
        gate_tree.write(root, "docs/Spread.md", "`spread()` moves it.\n")
        gate_tree.write(root, "docs/plans/Next.md", "`find_level()` will change.\n")
        for command in (["init", "-q"], ["add", "."],
                        ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "base"]):
            subprocess.run(["git", *command], cwd=root, check=True, capture_output=True)

    def test_citers_report_docs_citing_the_touched_function_only(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.citers_repo(root)
            source = root / "launcher/main/liquid.c"
            source.write_text(source.read_text().replace("return x;", "return x * 2;"))
            found = doc_citers.citers(root, ["launcher/main/liquid.c"], ["HEAD"])
        self.assertEqual(found, {"launcher/main/liquid.c": {"docs/Liquid.md": ["find_level()"]}})

    def test_citers_fall_back_to_path_citations_when_no_symbol_is_touched(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.citers_repo(root)
            source = root / "launcher/main/liquid.c"
            source.write_text("/* liquid levels */\n" + source.read_text())
            found = doc_citers.citers(root, ["launcher/main/liquid.c"], ["HEAD"])
        self.assertEqual(found, {"launcher/main/liquid.c": {"docs/Liquid.md": ["liquid.c"]}})

    def test_citers_skip_a_source_whose_diff_is_empty(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.citers_repo(root)
            found = doc_citers.citers(root, ["launcher/main/liquid.c"], ["HEAD"])
        self.assertEqual(found, {})

    def cmake_repo(self, root):
        gate_tree.write(root, "launcher/main/CMakeLists.txt",
                   "set(app_srcs main.c)\n"
                   "idf_component_register(SRCS ${app_srcs} WHOLE_ARCHIVE)\n")
        gate_tree.write(root, "docs/Build.md", "`WHOLE_ARCHIVE` keeps unreferenced apps linked.\n")
        gate_tree.write(root, "docs/Glob.md", "Apps are globbed by `main/CMakeLists.txt`.\n")
        gate_tree.write(root, "docs/Other.md", "`CONFIGURE_DEPENDS` re-globs.\n")
        for command in (["init", "-q"], ["add", "."],
                        ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "base"]):
            subprocess.run(["git", *command], cwd=root, check=True, capture_output=True)

    def test_citers_report_a_cmake_keyword_on_a_changed_line_and_the_path(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.cmake_repo(root)
            source = root / "launcher/main/CMakeLists.txt"
            source.write_text(source.read_text().replace(" WHOLE_ARCHIVE)", ")"))
            found = doc_citers.citers(root, ["launcher/main/CMakeLists.txt"], ["HEAD"])
        self.assertEqual(found, {"launcher/main/CMakeLists.txt": {
            "docs/Build.md": ["WHOLE_ARCHIVE"], "docs/Glob.md": ["main/CMakeLists.txt"]}})

    def test_citers_skip_a_cmake_file_whose_diff_is_empty(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.cmake_repo(root)
            found = doc_citers.citers(root, ["launcher/main/CMakeLists.txt"], ["HEAD"])
        self.assertEqual(found, {})

    def test_citers_report_a_script_path_even_when_a_function_is_touched(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            gate_tree.write(root, "launcher/tools/gen_font.py", "def render(size):\n    return size\n")
            gate_tree.write(root, "launcher/tools/flash.sh", "flash() {\n    echo flash\n}\n")
            gate_tree.write(root, "docs/Fonts.md", "Run `tools/gen_font.py`.\n")
            gate_tree.write(root, "docs/Render.md", "`render()` rasterizes.\n")
            gate_tree.write(root, "docs/Flash.md", "Run `flash.sh`.\n")
            for command in (["init", "-q"], ["add", "."],
                            ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "base"]):
                subprocess.run(["git", *command], cwd=root, check=True, capture_output=True)
            script = root / "launcher/tools/gen_font.py"
            script.write_text(script.read_text().replace("return size", "return size * 2"))
            shell = root / "launcher/tools/flash.sh"
            shell.write_text(shell.read_text().replace("echo flash", "echo flashing"))
            found = doc_citers.citers(root, ["launcher/tools/gen_font.py", "launcher/tools/flash.sh"], ["HEAD"])
        self.assertEqual(found, {
            "launcher/tools/gen_font.py": {"docs/Fonts.md": ["tools/gen_font.py"], "docs/Render.md": ["render()"]},
            "launcher/tools/flash.sh": {"docs/Flash.md": ["flash.sh"]}})

    def test_a_cmake_path_citation_must_exist(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.cmake_repo(root)
            gate_tree.write(root, "docs/Gone.md", "See `tools/CMakeLists.txt`.\n")
            missing = [c.value for c in check_doc_citations.citations(root)
                       if c.kind == "path" and not check_doc_citations.path_exists(root, c.value)]
        self.assertEqual(missing, ["tools/CMakeLists.txt"])

    def test_reverse_index_ignores_repeated_function_definition(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=root, check=True)
            gate_tree.write(root, "launcher/main/a.c", "void fixture(void) {}\n")
            gate_tree.write(root, "launcher/main/b.c", "void fixture(void) {}\n")
            gate_tree.write(root, "docs/Guide.md", "Use `fixture()`.\n")
            gate_tree.write(root, "docs/Bound.md", "Use `fixture()` from `launcher/main/a.c`.\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=root, check=True)
            base = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                                  check=True, capture_output=True, text=True).stdout.strip()
            gate_tree.write(root, "launcher/main/a.c", "void fixture(void) { int changed = 1; }\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "change"], cwd=root, check=True)
            result = subprocess.run(
                [sys.executable, str(SCRIPTS / "docs_touched_by.py"), base, "--json"],
                cwd=root, check=True, capture_output=True, text=True)
        self.assertEqual(json.loads(result.stdout)["citations"], [{
            "doc": "docs/Bound.md", "kind": "function", "line": 1,
            "symbol": "fixture",
        }])


if __name__ == "__main__":
    unittest.main()
