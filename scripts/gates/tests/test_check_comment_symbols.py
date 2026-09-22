"""Regression tests for scripts/gates/check_comment_symbols.py."""
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import check_comment_symbols  # noqa: E402


class StaleForeignTest(unittest.TestCase):
    def write(self, root, path, text):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def test_a_foreign_entry_no_comment_cites_is_reported_stale(self):
        # FOREIGN is meant to name real vendor/libc functions a comment
        # legitimately cites with no local definition to find - an entry
        # nothing ever cites any more is exactly the dead-allowlist trap
        # check_doc_citations.py's own stale_allowlist() already guards
        # against.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* calls malloc() to get memory */\n"
                      "void gfx_init(void) {}\n")
            with mock.patch.object(check_comment_symbols, "FOREIGN",
                                   {"malloc", "an_unused_foreign_entry"}):
                stale = check_comment_symbols.stale_foreign(str(root))
        self.assertEqual(stale, ["an_unused_foreign_entry"])

    def test_a_cited_foreign_entry_is_not_reported_stale(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* calls malloc() to get memory */\n"
                      "void gfx_init(void) {}\n")
            with mock.patch.object(check_comment_symbols, "FOREIGN", {"malloc"}):
                stale = check_comment_symbols.stale_foreign(str(root))
        self.assertEqual(stale, [])


if __name__ == "__main__":
    unittest.main()
