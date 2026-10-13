"""compile_db's reading of a compiler's -M/-MM dependency output."""
import os
import pathlib
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))

from anim import track_host  # noqa: E402
from anim_probe import has_compiler  # noqa: E402
from build import compile_db  # noqa: E402


class ParseDependenciesTests(unittest.TestCase):
    def test_continuations_join_and_the_target_is_dropped(self):
        text = "deps: a.c \\\n b.h \\\r\n  c.h\n"
        self.assertEqual(compile_db.parse_dependencies(text, "d"), [os.path.abspath(f"d/{name}") for name in ("a.c", "b.h", "c.h")])

    def test_escaped_spaces_hashes_and_dollars_stay_in_one_name(self):
        self.assertEqual(compile_db.parse_dependencies(r"x: my\ dir/a\#b.h $$v.h", "d"),
                         [os.path.abspath("d/my dir/a#b.h"), os.path.abspath("d/$v.h")])

    def test_every_rule_of_a_multi_source_listing_counts(self):
        self.assertEqual(compile_db.parse_dependencies("x: /a.c\nx: /b.c\n"), [os.path.abspath("/a.c"), os.path.abspath("/b.c")])


@unittest.skipUnless(has_compiler(), "needs sh and a C compiler")
class DependenciesTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = pathlib.Path(temp.name)
        (self.root / "inc dir").mkdir()
        (self.root / "inc dir" / "h.h").write_text("\n")
        (self.root / "a.c").write_text('#include "h.h"\n')

    def entry(self, *flags):
        return {"directory": str(self.root), "file": "a.c", "arguments": [track_host.compiler(), *flags, "-c", "a.c"]}

    def test_a_header_under_a_path_with_a_space_is_listed(self):
        found = compile_db.dependencies(self.entry("-I", "inc dir"))
        self.assertIn(os.path.abspath(self.root / "inc dir" / "h.h"), found)

    def test_a_failing_scan_raises_with_the_compiler_s_words(self):
        (self.root / "a.c").write_text("#error stop\n")
        with self.assertRaisesRegex(RuntimeError, "dependency scan failed"):
            compile_db.dependencies(self.entry())


if __name__ == "__main__":
    unittest.main()
