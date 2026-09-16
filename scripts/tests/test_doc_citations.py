"""Regression tests for documentation citation tools."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import check_doc_citations  # noqa: E402
import check_doc_constants  # noqa: E402
import check_doc_vocabulary  # noqa: E402
import doc_drift  # noqa: E402


class DocCitationTest(unittest.TestCase):
    def write(self, root, path, text):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def fixture(self, root):
        self.write(root, "launcher/main/example.c",
                   "void live_function(void) {}\n#define LIVE_MACRO 1\n")
        self.write(root, "scripts/live.py", "def helper():\n    pass\n")
        self.write(root, "docs/Guide.md", """`live_function()` `missing_function()`
`example.c` `missing.h` `LIVE_MACRO` `LIVE_MISSING`
```sh
missing_function() LIVE_MISSING missing.sh
```
""")

    def test_checker_reports_only_missing_repo_citations(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.fixture(root)
            missing = check_doc_citations.check(root)
        self.assertEqual(
            [(item.kind, item.value) for item in missing],
            [("function", "missing_function"), ("path", "missing.h"),
             ("macro", "LIVE_MISSING")],
        )

    def test_tokenizer_ignores_pasted_identifier_fragments(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/example.c", "void live_function(void) {}\n")
            self.write(root, "docs/Guide.md",
                       "`live_function()` `_count()` `palette##_set_lut16()` `<prefix>_count()`\n")
            found = list(check_doc_citations.citations(root))
        self.assertEqual([(item.kind, item.value) for item in found],
                         [("function", "live_function")])

    def test_constant_checker_matches_and_reports_mismatches(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/example.h", "#define LIVE_LIMIT 32\nenum { LIVE_ENUM = 9 };\n")
            self.write(root, "docs/Guide.md",
                       "`LIVE_LIMIT` is 32. `LIVE_ENUM` is 9.\n"
                       "`LIVE_LIMIT` is 16.\n| `LIVE_LIMIT` | 16 |\n")
            found = check_doc_constants.check(root)
        self.assertEqual([(item.line, item.name, item.claimed, item.defined) for item in found],
                         [(2, "LIVE_LIMIT", 16, 32), (3, "LIVE_LIMIT", 16, 32)])

    def test_constant_checker_skips_ambiguous_historical_and_allowlisted_values(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/a.h", "#define AMBIGUOUS 4\n#define LIVE_LIMIT 32\n")
            self.write(root, "launcher/main/b.h", "#define AMBIGUOUS 8\n")
            self.write(root, "docs/Guide.md",
                       "`AMBIGUOUS` is 1.\n`LIVE_LIMIT` was 16.\n"
                       "`LIVE_LIMIT` is 16.\n`LIVE_LIMIT` is 8. <!-- doc-constants: ignore -->\n")
            self.write(root, "scripts/doc_constant_allowlist.txt",
                       "docs/Guide.md\tLIVE_LIMIT\t16\tdeliberate exception\n")
            found = check_doc_constants.check(root)
        self.assertEqual(found, [])

    def test_constant_checker_checks_current_claim_beside_a_hypothetical(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/example.h", "#define LIVE_LIMIT 32\n")
            self.write(root, "docs/Guide.md",
                       "`LIVE_LIMIT` is 16. At `LIVE_LIMIT` = 8 it would fail.\n")
            found, skipped = check_doc_constants.check(root, verbose=True)
        self.assertEqual([(item.name, item.claimed, item.defined) for item in found], [
            ("LIVE_LIMIT", 16, 32),
        ])
        self.assertIn(("docs/Guide.md", 1, "skipped-on-hypothetical"), skipped)

    def test_constant_checker_ignores_a_hypothetical_only_claim(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/example.h", "#define LIVE_LIMIT 32\n")
            self.write(root, "docs/Guide.md",
                       "At `LIVE_LIMIT` = 16 the system would fail.\n")
            found, skipped = check_doc_constants.check(root, verbose=True)
        self.assertEqual(found, [])
        self.assertIn(("docs/Guide.md", 1, "skipped-on-hypothetical"), skipped)

    def test_constant_checker_requires_an_explicit_constant_value_claim(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/example.h", "#define back 1\n#define LIVE_LIMIT 32\n")
            self.write(root, "docs/Guide.md", """\
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
            self.write(root, "launcher/main/example.h", "#define CONDUCT_REACH 32\n#define FRAME_MS 16\n")
            self.write(root, "launcher/main/apps/sand/material.c", """\
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
            self.write(root, "docs/Guide.md", """\
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
            self.write(root, "launcher/main/example.c", "void old_name(void) {}\n")
            self.write(root, "docs/Guide.md", "Use `old_name()`.\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=root, check=True)
            base = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                                  check=True, capture_output=True, text=True).stdout.strip()
            self.write(root, "launcher/main/example.c", "void new_name(void) {}\n")
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
            self.write(root, "docs/Guide.md", "Guide\n")
            self.write(root, "launcher/main/example.c", "void live_function(void) {}\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=root, check=True)
            self.write(root, "docs/doc_review_ledger.txt", "docs/Guide.md\t2026-09-17\tchecked\n")
            rows = doc_drift.report(root, today=__import__("datetime").date(2026, 9, 18))
        guide = next(row for row in rows if row["doc"] == "docs/Guide.md")
        self.assertEqual(guide["age"], 1)

    def test_drift_ranks_changed_cited_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=root, check=True)
            self.write(root, "docs/Guide.md", "See `launcher/main/example.c`.\n")
            self.write(root, "launcher/main/example.c", "int example = 1;\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=root, check=True)
            self.write(root, "launcher/main/example.c", "int example = 2;\n")
            subprocess.run(["git", "commit", "-am", "change", "-q"], cwd=root, check=True)
            row = next(row for row in doc_drift.report(root) if row["doc"] == "docs/Guide.md")
        self.assertEqual((len(row["files"]), row["rank"] - row["age"]), (1, 30))

    def test_vocabulary_gate_honours_archive_exception(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/doc_vocabulary.txt", "C6\twrong board\n")
            self.write(root, "scripts/doc_vocabulary_exceptions.txt", "docs/Archive.md\tarchive\n")
            self.write(root, "docs/Guide.md", "C6 is current.\n")
            self.write(root, "docs/Archive.md", "C6 is historical.\n")
            found = check_doc_vocabulary.check(root)
        self.assertEqual([(path, term) for path, _, term, _ in found], [("docs/Guide.md", "C6")])

    def test_vocabulary_gate_honours_inline_and_previous_line_escapes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/doc_vocabulary.txt", "C6\twrong board\n")
            self.write(root, "docs/Guide.md",
                       "C6 is historical. <!-- doc-vocabulary: ignore -->\n"
                       "<!-- doc-vocabulary: ignore -->\n"
                       "C6 was the prior target.\n")
            found = check_doc_vocabulary.check(root)
        self.assertEqual(found, [])

    def test_vocabulary_gate_reports_stale_exception(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/doc_vocabulary.txt", "C6\twrong board\n")
            self.write(root, "scripts/doc_vocabulary_exceptions.txt",
                       "docs/Deleted.md\tobsolete exception\n")
            found = check_doc_vocabulary.check(root)
        self.assertEqual([(path, term) for path, _, term, _ in found], [
            ("docs/Deleted.md", "stale exception"),
        ])

    def test_vocabulary_gate_reports_unescaped_retired_term(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/doc_vocabulary.txt", "C6\twrong board\n")
            self.write(root, "docs/Guide.md", "C6 is not the current board.\n")
            found = check_doc_vocabulary.check(root)
        self.assertEqual([(path, term) for path, _, term, _ in found], [("docs/Guide.md", "C6")])

    def test_reverse_index_ignores_repeated_function_definition(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=root, check=True)
            self.write(root, "launcher/main/a.c", "void fixture(void) {}\n")
            self.write(root, "launcher/main/b.c", "void fixture(void) {}\n")
            self.write(root, "docs/Guide.md", "Use `fixture()`.\n")
            self.write(root, "docs/Bound.md", "Use `fixture()` from `launcher/main/a.c`.\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=root, check=True)
            base = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                                  check=True, capture_output=True, text=True).stdout.strip()
            self.write(root, "launcher/main/a.c", "void fixture(void) { int changed = 1; }\n")
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
