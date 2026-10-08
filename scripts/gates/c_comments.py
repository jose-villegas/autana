"""C comment and literal spans, prose and source selection for repository gates."""
import pathlib
import re

from check_generated_files import generated_files
from tracked import tracked_files, committable

# Vendored upstream (microui) and every file whose banner says it is
# generated: neither is ours to rewrite, and the generators' banner comments
# would dominate the report.
EXCLUDED = ("launcher/components/", "launcher/test/framework/", *generated_files())


# A drawn rule's run: 3+ of `=`, `_`, `#` or `-`; this tree's own
# "/* --- title ---- */" padding is exactly 3 dashes, but `*` alone needs
# 4+, since a bare "***" is style(9) emphasis, not decoration.
RULE_RUN = r"(?:[=_#\-]{3,}|[=*_#\-]{4,})"


class Comment:
    def __init__(self, path, line, kind, start=0, end=0):
        self.path = path
        self.line = line
        self.kind = kind  # "block" or "line"
        self.raw_lines = []
        self.spans = [(start, end)]
        self.first = False

    @property
    def text(self):
        """The comment's prose: decoration stripped, paragraphs rejoined."""
        stripped = []
        for raw in self.raw_lines:
            s = raw.strip()
            if self.kind == "line":
                s = re.sub(r"^//+", "", s)
            else:
                s = re.sub(r"^/\*+", "", s)
                s = re.sub(r"\*+/$", "", s)
                s = re.sub(r"^\*+", "", s)
            stripped.append(s.strip())
        return " ".join(p for p in stripped if p)

    @property
    def length(self):
        return len(self.text)

    @property
    def lines(self):
        """How tall the comment is. A file header is judged on this rather
        than on character count, since it describes a whole module and the
        character rule is aimed at comments beside code."""
        return len(self.raw_lines)

    @property
    def line_range(self):
        return range(self.line, self.line + len(self.raw_lines))

    @property
    def has_rule(self):
        """Opens with a drawn rule (`/*====`, `//----`) or draws one on the
        same line as its own text: `/* --- title ---- */`. Decoration this
        tree does not use; scripts/gates/strip_comment_rules.py finds any that
        returns."""
        first = self.raw_lines[0].strip()
        if re.match(r"^/\*\s?[=*\-_#]{4,}", first) or re.match(r"^//\s*[=*\-_#]{4,}", first):
            return True
        body = first[2:]
        if body.endswith("*/"):
            body = body[:-2]
        body = body.strip()
        return bool(re.match("^" + RULE_RUN + r"\s", body) or re.search(r"\s" + RULE_RUN + "$", body))

    @property
    def is_banner(self):
        """The file's header: its first comment, wherever it sits.

        Only meaningful when the whole file was scanned. An edit fragment has
        a first comment too, and it is rarely the file's; code holding a
        fragment has to find the header on disk.
        """
        return self.first


def balanced_end(source, start, opening="{", closing="}"):
    """Offset after a balanced delimiter pair; callers own literal masking."""
    depth = 0
    for at in range(start, len(source)):
        depth += (source[at] == opening) - (source[at] == closing)
        if depth == 0:
            return at + 1
    raise ValueError("unclosed " + opening)


def tokens(source, literals=True):
    """Comment and literal spans; escaped quotes cannot start a comment."""
    i, n = 0, len(source)
    while i < n:
        start = i
        if literals and source[i] in "\"'":
            quote = source[i]
            i += 1
            while i < n and source[i] != quote:
                i += 2 if source[i] == "\\" and i + 1 < n else 1
            i = min(i + 1, n)
            yield "literal", start, i
        elif source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = n if end < 0 else end + 2
            yield "block", start, i
        elif source.startswith("//", i):
            end = source.find("\n", i)
            i = n if end < 0 else end
            yield "line", start, i
        else:
            i += 1


def scan(path, source):
    """Yield every comment in `source`, skipping string and char literals."""
    comments = []
    at, line = 0, 1
    for kind, i, end in tokens(source):
        line += source.count("\n", at, i)
        at = end
        if kind == "literal":
            line += re.sub(r"\\.", "", source[i:end], flags=re.S).count("\n")
            continue
        if kind == "block":
            start_of_line = source.rfind("\n", 0, i) + 1
            own_line = source[start_of_line:i].strip() == ""
            prev = comments[-1] if comments else None
            mergeable = (
                own_line
                and prev is not None
                and prev.kind == "block"
                and prev.own_line
                and prev.line + len(prev.raw_lines) == line
            )
            if mergeable:
                prev.raw_lines += source[i:end].split("\n")
                prev.spans.append((i, end))
            else:
                com = Comment(path, line, "block", i, end)
                com.own_line = own_line
                com.raw_lines = source[i:end].split("\n")
                comments.append(com)
            line += source.count("\n", i, end)
        elif kind == "line":
            start_of_line = source.rfind("\n", 0, i) + 1
            own_line = source[start_of_line:i].strip() == ""
            prev = comments[-1] if comments else None
            mergeable = (
                own_line
                and prev is not None
                and prev.kind == "line"
                and prev.own_line
                and prev.line + len(prev.raw_lines) == line
            )
            if mergeable:
                prev.raw_lines.append(source[i:end])
                prev.spans.append((i, end))
            else:
                com = Comment(path, line, "line", i, end)
                com.own_line = own_line
                com.raw_lines = [source[i:end]]
                comments.append(com)
    if comments:
        comments[0].first = True
    return comments


PRAGMA_ONCE = re.compile(r"\A\s*#\s*pragma\s+once[^\n]*\n")


def file_header(path, source):
    """The file's header comment: the first comment, when nothing but
    whitespace or a leading `#pragma once` comes before it. None when code
    comes first."""
    head = PRAGMA_ONCE.sub("", source, count=1).lstrip()
    if not head.startswith(("/*", "//")):
        return None
    comments = scan(path, source)
    return comments[0] if comments else None


def sources(root, *, tracked=False, extensions=(".c", ".h"), excluded=True, files=None):
    """C sources eligible for comment gates, with tracked-only selection optional."""
    root = pathlib.Path(root)
    paths = (root / name for name in tracked_files(root)) if tracked else committable(root)
    for path in paths:
        rp = path.relative_to(root).as_posix() if tracked else path.as_posix()
        if files is not None and path.as_posix() not in files:
            continue
        if path.suffix in extensions and (not excluded or not rp.startswith(EXCLUDED)):
            yield path


def blank_comments(text, mode="comments"):
    """Blank C comments and optionally literals, retaining offsets and newlines."""
    out, at = [], 0
    for kind, start, end in tokens(text, literals=mode != "spelled"):
        if kind == "literal" and mode != "code":
            continue
        out.append(text[at:start])
        out.append("".join(ch if ch == "\n" else " " for ch in text[start:end]))
        at = end
    out.append(text[at:])
    return "".join(out)
