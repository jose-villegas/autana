"""Fenced code blocks of a Markdown text: three or more backticks or tildes
after any quote markers and one list marker (a backtick fence's info string
holds no backtick). A fence closes on a line of the same character, at least
as long, with no info string, at its opener's quote depth and with no list
marker, or at the end of the text; nothing opens inside it. Unlike CommonMark,
indentation is not checked and a quoted fence outlives the end of its quote."""
import collections
import re

FENCE = re.compile(r"^\s*((?:>\s*)*)((?:[-+*]|[0-9]+[.)])\s+)?(`{3,}|~{3,})(.*)$")


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
            quotes, item, fence, info = match.groups()
            depth = quotes.count(">")
            if not opened:
                if fence[0] == "~" or "`" not in info:
                    opened = depth, fence, number, info.strip(), offset
            elif ((depth, fence[0]) == (opened[0], opened[1][0]) and len(fence) >= len(opened[1])
                    and not item and not info.strip()):
                yield Fence(*opened[2:], offset + len(line), number)
                opened = None
        offset += len(line)
    if opened:
        yield Fence(*opened[2:], offset, None)


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
