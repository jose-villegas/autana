"""check_math_docs.py: the page tracks the headers, in both directions."""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import check_math_docs  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]
FUNCTION = "    static inline int P##_add(int a) { return a; } \\\n"


def make_tree(root, page_extra="", template_extra="", header_extra="", skip_section=None, empty_template=None):
    """A minimal tree that agrees with itself; each argument breaks one rule."""
    math = root / "launcher/main/util/math"
    math.mkdir(parents=True)
    (root / "docs/math").mkdir(parents=True)
    for family in check_math_docs.FAMILIES:
        body = "#define MATH_DEFINE_X(P) \\\n" + FUNCTION + (template_extra if family == "vec2" else "")
        if family == empty_template:
            body = "#define MATH_DEFINE_X(P)\n"
        (math / (family + "_template.h")).write_text(body)
    (math / "vec2f.h").write_text("MATH_DEFINE_VEC2(vec2f, float)\n" + header_extra)
    page = "".join("### %s\n\n| `P_add(` |\n\n" % f for f in check_math_docs.FAMILIES if f != skip_section)
    page += "`vec2f`\n" + page_extra
    (root / "docs/math/README.md").write_text(page)


def problems_for(**kwargs):
    with tempfile.TemporaryDirectory() as d:
        make_tree(pathlib.Path(d), **kwargs)
        return check_math_docs.check(d)


class CheckMathDocs(unittest.TestCase):
    def test_the_real_page_matches_the_real_headers(self):
        self.assertEqual([], check_math_docs.check(REPO))

    def test_a_synthetic_tree_that_agrees_passes(self):
        self.assertEqual([], problems_for())

    def test_a_template_function_missing_from_the_page_fails(self):
        extra = "    static inline int P##_wobble(int a) { return a; } \\\n"
        self.assertTrue(any("P_wobble(" in p for p in problems_for(template_extra=extra)))

    def test_a_hand_written_function_the_page_lacks_fails(self):
        extra = "static inline int\nvec2f_wobble(int a) {\n    return a;\n}\n"
        self.assertTrue(any("vec2f_wobble" in p for p in problems_for(header_extra=extra)))

    def test_a_hand_written_function_the_page_names_passes(self):
        extra = "static inline int\nvec2f_wobble(int a) {\n    return a;\n}\n"
        self.assertEqual([], problems_for(header_extra=extra, page_extra="`vec2f_wobble(a)`\n"))

    def test_an_identity_initializer_the_page_lacks_fails(self):
        extra = "#define TRANSFORMQ_IDENTITY {0}\n"
        self.assertTrue(any("TRANSFORMQ_IDENTITY" in p for p in problems_for(header_extra=extra)))

    def test_an_instantiation_the_page_does_not_name_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            make_tree(root)
            (root / "launcher/main/util/math/vec2q.h").write_text("MATH_DEFINE_VEC2(vec2q, float)\n")
            self.assertTrue(any("vec2q" in p for p in check_math_docs.check(d)))

    def test_a_missing_family_section_fails(self):
        self.assertTrue(any("no `### quat` section" in p for p in problems_for(skip_section="quat")))

    def test_a_template_that_defines_nothing_fails_instead_of_passing_on_nothing(self):
        self.assertTrue(any("defines no functions" in p for p in problems_for(empty_template="mat4")))

    def test_a_page_entry_for_a_function_that_no_longer_exists_fails(self):
        self.assertTrue(any("vec2f_gone(" in p for p in problems_for(page_extra="`vec2f_gone(a)`\n")))

    def test_a_page_entry_for_an_existing_function_passes(self):
        self.assertEqual([], problems_for(page_extra="`vec2f_add(a, b)` and `P_add(a)`\n"))

    def test_code_fences_are_not_read_as_entries(self):
        self.assertEqual([], problems_for(page_extra="```c\nvec2f_gone(1);\n```\n"))


if __name__ == "__main__":
    unittest.main()
