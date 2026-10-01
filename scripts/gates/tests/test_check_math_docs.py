"""check_math_docs.py: the page tracks the headers, in both directions."""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import check_math_docs  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]


def make_tree(root, page_extra="", template_extra=""):
    math = root / "launcher/main/util/math"
    math.mkdir(parents=True)
    (root / "docs/math").mkdir(parents=True)
    for family in check_math_docs.SECTIONS:
        body = "#define MATH_DEFINE_X(P) \\\n    static inline int P##_add(int a) { return a; } \\\n" + (template_extra if family == "vec2" else "")
        (math / (family + "_template.h")).write_text(body)
    (math / "vec2f.h").write_text("MATH_DEFINE_VEC2(vec2f, float)\n")
    page = "".join("### %s\n\n| `P_add(` |\n\n" % f for f in check_math_docs.SECTIONS) + "`vec2f`\n" + page_extra
    (root / "docs/math/README.md").write_text(page)


class CheckMathDocs(unittest.TestCase):
    def test_the_real_page_matches_the_real_headers(self):
        self.assertEqual([], check_math_docs.check(REPO))

    def test_a_synthetic_tree_that_agrees_passes(self):
        with tempfile.TemporaryDirectory() as d:
            make_tree(pathlib.Path(d))
            self.assertEqual([], check_math_docs.check(d))

    def test_a_template_function_missing_from_the_page_fails(self):
        with tempfile.TemporaryDirectory() as d:
            make_tree(pathlib.Path(d), template_extra="    static inline int P##_wobble(int a) { return a; } \\\n")
            problems = check_math_docs.check(d)
            self.assertTrue(any("P_wobble(" in p for p in problems), problems)

    def test_an_instantiation_the_page_does_not_name_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root)
            (root / "launcher/main/util/math/vec2q.h").write_text("MATH_DEFINE_VEC2(vec2q, float)\n")
            self.assertTrue(any("vec2q" in p for p in check_math_docs.check(d)))


if __name__ == "__main__":
    unittest.main()
