#!/usr/bin/env python3
"""Fail when a documentation citation no longer resolves in this tree.

    python scripts/gates/check_doc_citations.py [--root ROOT]

A backticked function (`name()`), path or macro a doc cites must exist in
this tree, and a quoted section a doc or C comment cites must be a heading
of the doc it names. Under docs/plans/, which names what is not built yet,
names are not checked; section citations still are.

A line that cites a name from outside this tree on purpose - an SDK-private
function or header - carries ``<!-- doc-citations: ignore NAME -->``, NAME
spelled as it is between the backticks. It exempts that one name on that one
line; every other citation there is still checked. A marker whose name the
line does not cite, or whose name resolves after all, is itself reported,
and so is one that names nothing.
"""
import pathlib
import re
import sys

from check_comment_length import EXCLUDED as C_EXCLUDED, scan
from check_doc_index import blank_fences, doc_headings
from code_vocabulary import vocabulary
from tracked import tracked_files

INLINE = re.compile(r"`([^`\n]+)`")
FUNCTION = re.compile(r"^([a-z][a-z0-9_]*)\(\)$")
MACRO = re.compile(r"^[A-Z][A-Z0-9_]*$")
FILE = re.compile(r"^(?:launcher/|apps/|[\w.-]+/)*(?:[\w.-]+\.(?:c|h|py|sh|cmake|md)|CMakeLists\.txt)$")
MARKER = re.compile(r"<!-- doc-citations: ignore(?: ([^\s>]+))? -->")
PLANS = "docs/plans/"
SKIP_FENCES = {"sh", "shell", "bash", "console", "text", "output"}
FOREIGN_FUNCTIONS = {"exit", "main", "max", "name", "bsp_display_new"}
FOREIGN_PATHS = {"idf.py", "idf_tools.py"}
FOREIGN_MACRO_PREFIXES = ("ESP", "CONFIG_COMPILER", "CONFIG_LOG", "IDF", "SDMMC", "WHOLE", "LOG", "DP")

# A citation of one or more sections of a doc: `X.md`'s "Section", or "One"
# and "Two" in X.md. The backtick around the doc name is optional - both
# spellings appear in this tree. Quoted phrases are only allowed to pile up
# behind one doc reference, joined by "," or "and", matching how a comment
# citing two sections of the same doc actually reads.
QUOTED = re.compile(r'"([^"\n]{2,80})"')
_QUOTED_GROUP = r'(?:"[^"\n]{2,80}"\s*(?:,|and)?\s*)+'
SECTION_AFTER_DOC = re.compile(r"`?([A-Za-z0-9_./-]+\.md)`?'s\s+(" + _QUOTED_GROUP + r")")
SECTION_BEFORE_DOC = re.compile(r"(" + _QUOTED_GROUP + r")\s+in\s+`?([A-Za-z0-9_./-]+\.md)`?")


class Citation:
    def __init__(self, doc, line, kind, value, marked=False):
        self.doc = doc
        self.line = line
        self.kind = kind
        self.value = value
        self.marked = marked


class Marker:
    def __init__(self, doc, line, name, cited):
        self.doc = doc
        self.line = line
        self.name = name
        self.cited = cited


def documentation(root):
    root = pathlib.Path(root)
    yield from sorted((root / "docs").rglob("*.md"))
    for name in sorted(tracked_files(root, ["*.md"])):
        if "/" not in name:
            yield root / name


def cited_name(doc, number, text, marked=False):
    """The Citation backticked `text` makes, or None when it is no function,
    path or macro."""
    function = FUNCTION.fullmatch(text)
    if function:
        return Citation(doc, number, "function", function.group(1), marked)
    if FILE.fullmatch(text):
        return Citation(doc, number, "path", text, marked)
    if MACRO.fullmatch(text):
        return Citation(doc, number, "macro", text, marked)
    return None


def cited_lines(root):
    """(doc, line number, line) for every line whose names are checked:
    outside docs/plans/ and outside a shell or output fence."""
    root = pathlib.Path(root)
    for path in documentation(root):
        doc = path.relative_to(root).as_posix()
        if doc.startswith(PLANS):
            continue
        fenced = False
        skip = False
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if line.lstrip().startswith("```"):
                if not fenced:
                    language = line.lstrip()[3:].strip().lower()
                    skip = language in SKIP_FENCES
                    fenced = True
                else:
                    fenced = skip = False
                continue
            if not skip:
                yield doc, number, line


def citations(root, include_marked=False):
    """Every citation, without the ones a marker exempts unless
    `include_marked`."""
    for doc, number, line in cited_lines(root):
        marked = {match.group(1) for match in MARKER.finditer(line)}
        for text in INLINE.findall(line):
            citation = cited_name(doc, number, text, text in marked)
            if citation and (include_marked or not citation.marked):
                yield citation


def markers(root):
    """Every doc-citations marker, with whether its line cites its name as a
    function, path or macro."""
    for doc, number, line in cited_lines(root):
        for match in MARKER.finditer(line):
            name = match.group(1)
            cited = any(text == name and cited_name(doc, number, text)
                        for text in INLINE.findall(line))
            yield Marker(doc, number, name, cited)


def resolve_doc(root, value, citing_doc=None):
    """Resolve a citation's path the way this tree actually writes one -
    absolute from the repo root, `launcher/`- or `apps/`-rooted, a bare
    filename found anywhere, or (given the citing document) `../`/`./`
    relative to it. Returns the resolved Path or None. Shared by path
    citations and section citations so both agree on how "Guide.md" or
    "../Guide.md" finds a document."""
    root = pathlib.Path(root)
    if citing_doc and value.startswith(("../", "./")):
        resolved = (root / citing_doc).parent.joinpath(value).resolve()
        try:
            resolved.relative_to(root.resolve())
        except ValueError:
            return None
        return resolved if resolved.exists() else None
    if value.startswith("apps/"):
        candidate = root / "launcher/main" / value
        return candidate if candidate.exists() else None
    if "/" in value:
        for candidate in (root / value, root / "launcher" / value, root / "launcher/main" / value):
            if candidate.exists():
                return candidate
        for name in tracked_files(root):
            if ("/" + name).endswith("/" + value) and (root / name).exists():
                return root / name
        return None
    for name in tracked_files(root):
        if name.rsplit("/", 1)[-1] == value and (root / name).exists():
            return root / name
    return None


def path_exists(root, value, citing_doc=None):
    return resolve_doc(root, value, citing_doc) is not None


def _paragraphs(lines):
    """(start_line, joined_text) for each run of consecutive non-blank
    lines - the same unit a reader sees as one sentence, so a quoted
    section split across a wrapped line still reads as one citation."""
    start, buf = None, []
    for number, line in enumerate(lines, 1):
        if line.strip():
            if start is None:
                start = number
            buf.append(line.strip())
        elif buf:
            yield start, " ".join(buf)
            start, buf = None, []
    if buf:
        yield start, " ".join(buf)


class SectionCitation:
    def __init__(self, doc, line, target_doc, section):
        self.doc = doc
        self.line = line
        self.target_doc = target_doc
        self.section = section


def section_citations(root):
    """Every quoted-section citation - `X.md`'s "Section", or "One" and
    "Two" in X.md - across tracked docs and C comments."""
    root = pathlib.Path(root)
    found = []

    def scan_text(doc, line, text):
        for pattern, doc_group, sections_group in (
                (SECTION_AFTER_DOC, 1, 2), (SECTION_BEFORE_DOC, 2, 1)):
            for m in pattern.finditer(text):
                target_doc = m.group(doc_group)
                for section in QUOTED.findall(m.group(sections_group)):
                    found.append(SectionCitation(doc, line, target_doc, section))

    for path in documentation(root):
        rel = path.relative_to(root).as_posix()
        body = blank_fences(path.read_text(encoding="utf-8", errors="replace").splitlines())
        for start, text in _paragraphs(body):
            scan_text(rel, start, text)

    for rel in tracked_files(root, ["launcher/*.c", "launcher/*.h"]):
        path = root / rel
        if not path.exists():
            continue
        if any(rel.startswith(e) for e in C_EXCLUDED):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for c in scan(rel, text):
            scan_text(rel, c.line, c.text)

    return found


def unresolved_sections(root):
    """Every section citation whose target doc, or section within it, does
    not resolve."""
    root = pathlib.Path(root)
    heading_cache = {}
    missing = []
    for citation in section_citations(root):
        key = (citation.target_doc, citation.doc)
        if key not in heading_cache:
            target = resolve_doc(root, citation.target_doc, citation.doc)
            heading_cache[key] = doc_headings(target) if target else None
        heads = heading_cache[key]
        if heads is None:
            missing.append((citation, f"{citation.target_doc} does not resolve"))
        elif not any(h == citation.section or h.startswith(citation.section) for h in heads):
            missing.append((citation, f'"{citation.section}" is not a heading in {citation.target_doc}'))
    return missing


def unresolved(root):
    """Every unmarked citation that does not resolve."""
    return [citation for citation in _unresolved(root) if not citation.marked]


def stale_markers(root):
    """(marker, reason) for every marker that exempts nothing."""
    root = pathlib.Path(root)
    needed = {(c.doc, c.line, _spelling(c)) for c in _unresolved(root) if c.marked}
    stale = []
    for marker in markers(root):
        if marker.name is None:
            stale.append((marker, "names nothing"))
        elif not marker.cited:
            stale.append((marker, f"names {marker.name}, which this line does not cite"))
        elif (marker.doc, marker.line, marker.name) not in needed:
            stale.append((marker, f"names {marker.name}, which resolves"))
    return stale


def _spelling(citation):
    return citation.value + "()" if citation.kind == "function" else citation.value


def _unresolved(root):
    root = pathlib.Path(root)
    vocab = vocabulary(root)
    functions, macros = vocab.functions | vocab.script_functions, vocab.constants
    missing = []
    for citation in citations(root, include_marked=True):
        if citation.kind == "function" and (
                citation.value in FOREIGN_FUNCTIONS or
                citation.value.startswith(("esp_", "xTask", "vTask", "heap_caps_"))):
            continue
        if citation.kind == "path" and citation.value in FOREIGN_PATHS:
            continue
        if citation.kind == "macro":
            if citation.value.startswith(FOREIGN_MACRO_PREFIXES):
                continue
            prefix = citation.value.split("_", 1)[0]
            prefixes = {name.split("_", 1)[0] for name in macros}
            if "_" not in citation.value or prefix not in prefixes:
                continue
        exists = (citation.value in functions if citation.kind == "function" else
                  citation.value in macros if citation.kind == "macro" else
                  path_exists(root, citation.value, citation.doc))
        if not exists:
            missing.append(citation)
    return missing


def check(root):
    return unresolved(root)


def main(argv):
    root = pathlib.Path(".")
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = pathlib.Path(argv[1])
    elif argv:
        print("usage: check_doc_citations.py [--root ROOT]", file=sys.stderr)
        return 2
    try:
        missing = check(root)
        missing_sections = unresolved_sections(root)
        stale = stale_markers(root)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    for item in missing:
        label = _spelling(item)
        print(f"{item.doc}:{item.line}: missing {item.kind} citation {label}"
              f" (cited from outside this tree on purpose? end the line with"
              f" <!-- doc-citations: ignore {label} -->)")
    for citation, reason in missing_sections:
        print(f"{citation.doc}:{citation.line}: missing section citation - {reason}")
    for marker, reason in stale:
        print(f"{marker.doc}:{marker.line}: doc-citations marker {reason}")
    print(f"{len(missing_sections)} missing section citation"
          f"{'' if len(missing_sections) == 1 else 's'}")
    print(f"{len(missing)} missing documentation citation"
          f"{'' if len(missing) == 1 else 's'}")
    print(f"{len(stale)} stale doc-citations marker{'' if len(stale) == 1 else 's'}")
    return 1 if missing or missing_sections or stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
