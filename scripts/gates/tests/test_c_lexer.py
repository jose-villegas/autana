"""Shared C spans and code comparisons retain literal bytes and source lines."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from c_lexer import c_code, tokens
from c_comments import scan


class LexerTests(unittest.TestCase):
    def test_c_code_ignores_comments_but_preserves_literal_spacing(self):
        plain = 'char *s = "a  b";\nint x;\n'
        edited = '/* p */ char *s = "a  b"; // q\n  int x;\n'
        self.assertEqual(c_code(plain), c_code(edited))
        self.assertNotEqual(c_code(plain), c_code('char *s = "a b";\nint x;\n'))

    def test_unescaped_newline_ends_a_literal(self):
        text = "#error don't\n/* p */\n"
        spans = list(tokens(text))
        self.assertEqual([kind for kind, _, _ in spans], ["literal", "block"])
        self.assertEqual(text[spans[0][1]:spans[0][2]], "'t")
        self.assertEqual(scan("test.c", text)[0].line, 2)

    def test_line_comment_continues_through_backslash_newline(self):
        text = '// p\\\ncontinued\n/* q */\n'
        spans = list(tokens(text))
        self.assertEqual([kind for kind, _, _ in spans], ["line", "block"])
        self.assertEqual(text[spans[0][1]:spans[0][2]], '// p\\\ncontinued')
        comments = scan("test.c", text)
        self.assertEqual(comments[0].raw_lines, ['// p\\', 'continued'])
        self.assertEqual(comments[1].line, 3)

    def test_escaped_literal_newline_counts_in_comment_location(self):
        text = '"p\\\n/* q */";\n/* r */\n'
        comments = scan("test.c", text)
        self.assertEqual([comment.text for comment in comments], ["r"])
        self.assertEqual(comments[0].line, 3)

    def test_spliced_literal_comment_bytes_change_code(self):
        plain = '"p\\\n/* p */";\n/* r */\n'
        edited = plain.replace("/* p */", "/* q */")
        self.assertNotEqual(c_code(plain), c_code(edited))

    def test_literal_at_line_start_retains_newline(self):
        plain = '#define M \\\n"a"\nint x;\n'
        joined = '#define M \\\n"a" int x;\n'
        self.assertNotEqual(c_code(plain), c_code(joined))

    def test_merged_continued_line_comment_retains_physical_lines(self):
        text = '// p\n// q\\\ncontinued\n// r\n'
        comments = scan("test.c", text)
        self.assertEqual(len(comments), 1)
        self.assertEqual(comments[0].raw_lines,
                         ["// p", "// q\\", "continued", "// r"])
        self.assertEqual(comments[0].line, 1)

    def test_digit_separator_opens_no_literal(self):
        text = "int n = 1'000'000; /* p */ char c = 'x'; /* q */\n"
        self.assertEqual([c.text for c in scan("test.cpp", text)], ["p", "q"])
        hex_text = "auto m = 0xFF'FF; // r\n"
        self.assertEqual([c.text for c in scan("test.cpp", hex_text)], ["r"])

    def test_prefixed_char_literal_still_opens_a_literal(self):
        text = "auto c = u8'/'; /* p */\n"
        spans = list(tokens(text))
        self.assertEqual([kind for kind, _, _ in spans], ["literal", "block"])
        self.assertEqual(text[spans[0][1]:spans[0][2]], "'/'")

    def test_raw_string_hides_comment_markers_and_quotes(self):
        text = 'auto s = R"x(a "/* not */" b)x"; /* p */\nauto t = u8R"(// no)"; // q\n'
        self.assertEqual([c.text for c in scan("test.cpp", text)], ["p", "q"])
        spans = [text[a:b] for kind, a, b in tokens(text) if kind == "literal"]
        self.assertEqual(spans, ['"x(a "/* not */" b)x"', '"(// no)"'])

    def test_raw_string_spans_lines(self):
        text = 'auto s = R"(\n/* not */\n)";\n/* p */\n'
        comments = scan("test.cpp", text)
        self.assertEqual([c.text for c in comments], ["p"])
        self.assertEqual(comments[0].line, 4)

    def test_identifier_ending_in_r_is_no_raw_prefix(self):
        text = 'f(BAR"/* x */");\n'
        self.assertEqual([kind for kind, _, _ in tokens(text)], ["literal"])


if __name__ == "__main__":
    unittest.main()
