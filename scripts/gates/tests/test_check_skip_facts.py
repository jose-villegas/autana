"""Regression fixtures for the simulation skip-fact gate."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import check_skip_facts
import c_comments
import check_doc_constants


class SkipFactsTest(unittest.TestCase):
    def problems(self, body, declaration="SAND_FACT bool may_have_x;", extra=""):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "sand.h").write_text(
                "typedef struct { " + declaration + " } sand_t;\n", encoding="utf-8")
            (root / "sand.c").write_text(extra + "\nvoid step(sand_t *s) { " + body + " }\n",
                                         encoding="utf-8")
            return check_skip_facts.problems(root)

    def test_wrapped_read_passes(self):
        self.assertEqual(self.problems("if (SAND_SKIP_IF(!s->may_have_x)) return;"), [])

    def test_rule_read_passes(self):
        self.assertEqual(self.problems("if (SAND_FACT_RULE(s->may_have_x)) act();"), [])

    def test_bare_read_fails(self):
        self.assertTrue(any("may_have_x read outside SAND_SKIP_IF" in p
                            for p in self.problems("if (!s->may_have_x) return;")))

    def test_unmarked_field_fails(self):
        self.assertTrue(any("may_have_x missing SAND_FACT" in p
                            for p in self.problems("", "bool may_have_x;")))

    def test_writes_pass(self):
        self.assertEqual(self.problems("s->may_have_x = true; s->may_have_x |= 1; "
                                       "s->may_have_x &= ~1; ++s->may_have_x;"), [])

    def test_deriving_a_fact_is_a_write(self):
        self.assertEqual(self.problems("SAND_FACT bool cached = !s->may_have_x; "
                                       "if (SAND_SKIP_IF(cached)) return;"), [])
        self.assertEqual(self.problems("s->may_have_y = s->may_have_x;",
                                       "SAND_FACT bool may_have_x; SAND_FACT bool may_have_y;"), [])

    def test_a_derived_fact_is_still_checked(self):
        self.assertTrue(any("cached read outside SAND_SKIP_IF" in p
                            for p in self.problems("SAND_FACT bool cached = !s->may_have_x; "
                                                   "if (cached) return;")))

    def test_assignment_to_a_plain_local_is_a_read(self):
        self.assertTrue(self.problems("bool plain = s->may_have_x; if (plain) return;"))

    def test_block_in_a_comment_is_not_a_declaration(self):
        self.assertEqual(self.problems("", extra="/*\n#define BLOCK_NEAR 0x10u\n*/"), [])

    def test_shifted_bit_requires_marker(self):
        self.assertTrue(self.problems("", extra="#define BLOCK_NEAR (1u << 3)"))

    def test_macro_replacement_does_not_hide_a_read(self):
        self.assertTrue(self.problems("", extra="#define BODY(s) ((s)->may_have_x)"))

    def test_balanced_end_supports_gate_spans_and_initializer_owner(self):
        self.assertEqual(c_comments.balanced_end("f((x), y) tail", 1, "(", ")"), 9)
        self.assertEqual(c_comments.balanced_end("a[b[c]] tail", 1, "[", "]"), 7)

        self.assertEqual(check_doc_constants.braced_body("a = {1, {2, 3}};", 0), "1, {2, 3}")
        self.assertIsNone(check_doc_constants.braced_body("a = {1, {2, 3}", 0))

    def test_writer_and_predicate_bodies_pass(self):
        self.assertEqual(self.problems("", extra=
            "SAND_FACT static bool quiet(sand_t *s) { return !s->may_have_x; }\n"
            "SAND_FACT_WRITER void merge(sand_t *s) { s->may_have_x |= quiet(s); }"), [])

    def test_predicate_call_is_fact(self):
        self.assertTrue(self.problems("if (quiet(s)) return;", extra=
            "SAND_FACT static bool quiet(sand_t *s) { return !s->may_have_x; }"))

    def test_nested_multiline_argument_passes(self):
        self.assertEqual(self.problems("if (SAND_SKIP_IF(\n!s->may_have_x && (1))) return;"), [])

    def test_comments_and_strings_are_not_reads(self):
        self.assertEqual(self.problems('/* may_have_x */ puts("may_have_x");'), [])

    def test_coverage_sums_captures_and_fails_on_an_untested_site(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp) / "sand"
            root.mkdir()
            (root / "sand.c").write_text("void f(void) {\nif (SAND_SKIP_IF(a)) return;\n"
                                         "if (SAND_SKIP_IF(b)) return;\n}\n", encoding="utf-8")
            normal = pathlib.Path(temp) / "normal.txt"
            forced = pathlib.Path(temp) / "forced.txt"
            normal.write_text("SAND_SKIP x/sand/sand.c:2 3\n", encoding="utf-8")
            forced.write_text("SAND_SKIP x/sand/sand.c:3 0\n", encoding="utf-8")
            self.assertEqual(check_skip_facts.coverage(root, [normal, forced]), 1)
            forced.write_text("SAND_SKIP x/sand/sand.c:3 5\n", encoding="utf-8")
            self.assertEqual(check_skip_facts.coverage(root, [normal, forced]), 0)

    def test_block_define_requires_marker(self):
        self.assertTrue(self.problems("", extra="#define BLOCK_NEAR 0x10u"))
        self.assertEqual(self.problems("if (SAND_SKIP_IF(BLOCK_NEAR == 0)) return;",
                                      extra="#define BLOCK_NEAR 0x10u /* SAND_FACT */"), [])


if __name__ == "__main__":
    unittest.main()
