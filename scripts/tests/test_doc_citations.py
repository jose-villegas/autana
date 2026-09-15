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
