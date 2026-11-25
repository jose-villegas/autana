"""Fenced code blocks of a Markdown text, opened and closed as CommonMark does:
three or more backticks or tildes after any quote markers and one list marker
(a backtick fence's info string holds no backtick), closed by a line of the
same character, at least as long, with no info string, or by the end of the
text. Nothing opens inside a fence, so a shorter one nested in it is text."""
import collections
import re

FENCE = re.compile(r"^\s*(?:>\s*)*(?:(?:[-+*]|[0-9]+[.)])\s+)?(`{3,}|~{3,})(.*)$")


class Fence(collections.namedtuple("Fence", "line info start end close")):
    """Opening line number, stripped info string, offsets of the first and one
    past the last character, and closing line number (None if the text ends first)."""
    language = property(lambda self: (self.info.split() or [""])[0].lower())


def fences(text):
    """Every fenced block of `text`, numbering lines from 1 as `splitlines()` does."""
    opened, offset = None, 0
    for number, line in enumerate(text.splitlines(keepends=True), 1):
        match = FENCE.match(line)
        if match:
            fence, info = match.groups()
            if not opened:
                if fence[0] == "~" or "`" not in info:
                    opened = fence, number, info.strip(), offset
            elif fence[0] == opened[0][0] and len(fence) >= len(opened[0]) and not info.strip():
                yield Fence(*opened[1:], offset + len(line), number)
                opened = None
        offset += len(line)
    if opened:
        yield Fence(*opened[1:], offset, None)


def fenced_lines(text):
    """{line number: Fence} for every line of every fenced block, its fences included."""
    lines = {}
    for fence in fences(text):
        count = len(text[fence.start:fence.end].splitlines())
        lines.update(dict.fromkeys(range(fence.line, fence.line + count), fence))
    return lines


def blank_fences(lines):
    """`lines` with each fenced block's lines emptied, so line numbers still
    match the file and a shell transcript's Markdown is never read as prose."""
    fenced = fenced_lines("\n".join(lines))
    return ["" if number in fenced else line for number, line in enumerate(lines, 1)]
