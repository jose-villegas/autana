"""Shared C spans and code comparisons retain literal bytes and source lines."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from c_lexer import c_code, tokens
from c_comments import scan
from check_comment_length import code_only


class LexerTests(unittest.TestCase):
    def test_code_only_ignores_comments_but_preserves_literal_spacing(self):
        plain = 'char *s = "a  b";\nint x;\n'
        edited = '/* p */ char *s = "a  b"; // q\n  int x;\n'
        self.assertEqual(c_code(plain), c_code(edited))
        self.assertEqual(code_only(plain), code_only(edited))
        self.assertNotEqual(code_only(plain), code_only('char *s = "a b";\nint x;\n'))

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
        text = '"p\\\nq";\n/* r */\n'
        self.assertEqual(scan("test.c", text)[0].line, 3)


if __name__ == "__main__":
    unittest.main()
