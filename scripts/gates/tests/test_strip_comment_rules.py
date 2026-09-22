"""Regression tests for scripts/gates/strip_comment_rules.py."""
import pathlib
import sys
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import strip_comment_rules  # noqa: E402


class SameLineRuleTest(unittest.TestCase):
    def test_a_rule_drawn_on_the_same_line_as_its_text_is_detected(self):
        # The form this tree actually uses for a drawn-rule heading: a short
        # leading run, the title, then padding out to the margin - all on one
        # line, unlike the multi-line "/*====" banner has_rule already finds.
        source = "void f(void) {\n"
        source += "/* --- suite --------------------------------------------------- */\n"
        source += "    int x = 1;\n"
        source += "}\n"
        comments = list(strip_comment_rules.scan("t.c", source))
        self.assertEqual(len(comments), 1)
        self.assertTrue(comments[0].has_rule,
                        "a same-line rule ('/* --- suite ---... */') must be "
                        "recognised as a drawn rule, the same as '/*====' is")

    def test_the_rewrite_strips_a_same_line_rule_and_keeps_its_title(self):
        source = ("void f(void) {\n"
                  "/* --- suite --------------------------------------------------- */\n"
                  "    int x = 1;\n"
                  "}\n")
        new = strip_comment_rules.rewrite("t.c", source)
        self.assertIsNotNone(new, "the rewrite must touch a file carrying only "
                                  "a same-line rule")
        self.assertNotIn("---", new)
        self.assertIn("/* suite */", new)

    def test_a_same_line_rule_beside_a_colon_and_prose_keeps_every_word(self):
        # suite_gfx.c's real shape: leading "---", a title with its own
        # punctuation, then a long trailing pad - every prose word must
        # survive, only the dash runs go.
        source = ("void f(void) {\n"
                  "/* --- gfx_blit_dither: image-over-live-content compositing ---- */\n"
                  "}\n")
        new = strip_comment_rules.rewrite("t.c", source)
        self.assertIsNotNone(new)
        self.assertIn("gfx_blit_dither: image-over-live-content compositing", new)
        self.assertNotIn("---", new)

    def test_a_multiline_rule_with_a_space_after_the_star_is_detected(self):
        # This tree's real multi-line banner shape includes a space:
        # "/* ====...", not the no-space "/*====..." has_rule already found.
        source = ("void f(void) {\n"
                  "/* =====================================================\n"
                  " * suite\n"
                  " * ===================================================== */\n"
                  "}\n")
        comments = list(strip_comment_rules.scan("t.c", source))
        self.assertEqual(len(comments), 1)
        self.assertTrue(comments[0].has_rule,
                        "'/* ====...' (with a space before the rule) must be "
                        "recognised as a drawn rule, the same as '/*====' is")
        new = strip_comment_rules.rewrite("t.c", source)
        self.assertIsNotNone(new)
        self.assertNotIn("=", new)
        self.assertIn("suite", new)

    def test_widening_to_three_does_not_eat_a_leading_triple_star_emphasis(self):
        # LEAD_RULE/TAIL_RULE dropped from a 4-char run to 3 to strip this
        # tree's real "---" padding - which must not also start stripping a
        # genuine "***" emphasis marker a prose line happens to open with.
        source = ("void f(void) {\n"
                  "/* =====================================================\n"
                  " * *** never remove this lock ***\n"
                  " * ===================================================== */\n"
                  "}\n")
        new = strip_comment_rules.rewrite("t.c", source)
        self.assertIsNotNone(new)
        self.assertIn("*** never remove this lock ***", new)


class OneDefinitionOfARuleTest(unittest.TestCase):
    def test_three_stars_around_text_is_emphasis_to_both_checks(self):
        # has_rule (the length gate) and the stripper must agree on what a
        # drawn rule is: a "***" pair is style(9) emphasis, not decoration.
        source = "void f(void) {\n/* *** VERY important *** */\n    int x = 1;\n}\n"
        comments = list(strip_comment_rules.scan("t.c", source))
        self.assertEqual(len(comments), 1)
        self.assertFalse(comments[0].has_rule,
                         "'/* *** VERY important *** */' is emphasis, not a drawn rule")
        self.assertIsNone(strip_comment_rules.rewrite("t.c", source))


if __name__ == "__main__":
    unittest.main()
