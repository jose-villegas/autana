"""The fence walker opens and closes fenced blocks as CommonMark does."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from markdown_fences import blank_fences, fenced_lines, fences  # noqa: E402


def spans(text):
    """(opening line, info, closing line, block text) per fenced block."""
    return [(f.line, f.info, f.close, text[f.start:f.end]) for f in fences(text)]


class FenceTests(unittest.TestCase):
    def test_a_tilde_fence_closes_only_on_tildes(self):
        text = "~~~ c\nint a;\n```\n~~~\nprose\n"
        self.assertEqual(spans(text), [(1, "c", 4, "~~~ c\nint a;\n```\n~~~\n")])

    def test_a_closing_fence_may_be_longer_but_not_shorter(self):
        text = "````\n```\nstill code\n`````\nprose\n"
        self.assertEqual(spans(text), [(1, "", 4, "````\n```\nstill code\n`````\n")])

    def test_a_fence_with_an_info_string_never_closes_one(self):
        self.assertEqual(spans("```\n```c\n```\n"), [(1, "", 3, "```\n```c\n```\n")])

    def test_a_fence_inside_a_list_item(self):
        text = "1. step\n\n   ```sh\n   make\n   ```\n2. next\n"
        self.assertEqual([(f.line, f.language, f.close) for f in fences(text)], [(3, "sh", 5)])

    def test_a_fence_inside_a_blockquote(self):
        text = "> quoted\n> ```c\n> int a;\n> ```\n"
        self.assertEqual([(f.line, f.language, f.close) for f in fences(text)], [(2, "c", 4)])

    def test_a_closer_must_sit_at_the_openers_quote_depth(self):
        text = "> ```\n```\n> code\n> ```\nprose\n"
        self.assertEqual([(f.line, f.close) for f in fences(text)], [(1, 4)])

    def test_a_list_item_never_closes_a_fence(self):
        text = "```\n- ```\nstill code\n```\n"
        self.assertEqual([(f.line, f.close) for f in fences(text)], [(1, 4)])

    def test_a_quoted_fence_outlives_its_quote(self):
        """A documented departure from CommonMark: no container ends a fence."""
        self.assertEqual([(f.line, f.close) for f in fences("> ```c\n> a\n\nprose\n")], [(1, None)])

    def test_a_shorter_fence_nests_as_text(self):
        text = "````markdown\n```mermaid\nA-->B\n```\n````\n```c\nint a;\n```\n"
        self.assertEqual([(f.line, f.language, f.close) for f in fences(text)],
                         [(1, "markdown", 5), (6, "c", 8)])

    def test_backticks_in_the_info_string_are_inline_code(self):
        self.assertEqual(spans("```a``` is code\ntext\n"), [])

    def test_an_unclosed_fence_runs_to_the_end(self):
        text = "prose\n```\ncode\n"
        self.assertEqual(spans(text), [(2, "", None, "```\ncode\n")])

    def test_offsets_count_crlf_line_endings(self):
        text = "a\r\n```\r\nb\r\n```\r\nc\r\n"
        [fence] = fences(text)
        self.assertEqual(text[fence.start:fence.end], "```\r\nb\r\n```\r\n")

    def test_fenced_lines_cover_each_line_with_its_fences(self):
        text = "a\n~~~\nb\n~~~\nc\n```\nd"
        self.assertEqual(sorted(fenced_lines(text)), [2, 3, 4, 6, 7])

    def test_blank_fences_keeps_line_positions(self):
        lines = ["a", "````", "```", "b", "```", "````", "c"]
        self.assertEqual(blank_fences(lines), ["a", "", "", "", "", "", "c"])


if __name__ == "__main__":
    unittest.main()
