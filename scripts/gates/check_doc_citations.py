#!/usr/bin/env python3
"""Fail when a documentation citation resolves neither in this tree nor in
ESP-IDF.

    python scripts/gates/check_doc_citations.py [--root ROOT] [--require-idf]

A backticked function (`name()`), path or CONSTANT_NAME a doc cites must be
defined in this tree (the vendored components under launcher/components/
included) or, for what the firmware uses but does not define, declared
by ESP-IDF or its toolchain's C library (idf_vocabulary.outside_vocabulary()).
ESP-IDF counts for every chip it supports, so a name declared only for
another chip resolves too: that is the check's known limit. A quoted
section a doc or C comment cites must be a heading of the doc it
names. Under docs/plans/, which names what is not built yet, names are not
checked; section citations still are.

Without an ESP-IDF checkout, a name this tree does not define cannot be
told apart from a typo, so it is counted, not failed, and one line says so.
--require-idf, which CI passes, makes a missing checkout, or a missing
toolchain C library beside it, an error instead.
"""
import pathlib
import re
import sys

from c_comments import EXCLUDED as C_EXCLUDED, scan
from check_doc_index import blank_fences, doc_headings
from code_vocabulary import vocabulary
from idf_vocabulary import not_verified_notice, outside_vocabulary, required_missing
from tracked import tracked_files

INLINE = re.compile(r"`([^`\n]+)`")
FUNCTION = re.compile(r"^([a-z][a-z0-9_]*)\(\)$")
MACRO = re.compile(r"^[A-Z][A-Z0-9_]*$")
FILE = re.compile(r"^(?:launcher/|apps/|[\w.-]+/)*(?:[\w.-]+\.(?:c|h|py|sh|cmake|md)|CMakeLists\.txt)$")
PLANS = "docs/plans/"
SKIP_FENCES = {"sh", "shell", "bash", "console", "text", "output"}

# A citation of one or more sections of a doc: `X.md`'s "Section", or "One"
# and "Two" in X.md. The backtick around the doc name is optional: both
# spellings appear in this tree. Quoted phrases are only allowed to pile up
# behind one doc reference, joined by "," or "and", matching how a comment
# citing two sections of the same doc actually reads.
QUOTED = re.compile(r'"([^"\n]{2,80})"')
_QUOTED_GROUP = r'(?:"[^"\n]{2,80}"\s*(?:,|and)?\s*)+'
SECTION_AFTER_DOC = re.compile(r"`?([A-Za-z0-9_./-]+\.md)`?'s\s+(" + _QUOTED_GROUP + r")")
SECTION_BEFORE_DOC = re.compile(r"(" + _QUOTED_GROUP + r")\s+in\s+`?([A-Za-z0-9_./-]+\.md)`?")


class Citation:
    def __init__(self, doc, line, kind, value):
        self.doc = doc
        self.line = line
        self.kind = kind
        self.value = value


def documentation(root):
    root = pathlib.Path(root)
    yield from sorted((root / "docs").rglob("*.md"))
    for name in sorted(tracked_files(root, ["*.md"])):
        if "/" not in name:
            yield root / name


def cited_name(doc, number, text):
    """The Citation backticked `text` makes, or None when it is no function,
    path or macro."""
    function = FUNCTION.fullmatch(text)
    if function:
        return Citation(doc, number, "function", function.group(1))
    if FILE.fullmatch(text):
        return Citation(doc, number, "path", text)
    if MACRO.fullmatch(text):
        return Citation(doc, number, "macro", text)
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


def citations(root):
    """Every function, path and macro citation a checked line makes."""
    for doc, number, line in cited_lines(root):
        for text in INLINE.findall(line):
            citation = cited_name(doc, number, text)
            if citation:
                yield citation


def resolve_doc(root, value, citing_doc=None):
    """Resolve a citation's path the way this tree actually writes one,
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
    lines; the same unit a reader sees as one sentence, so a quoted
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
    """Every quoted-section citation (`X.md`'s "Section", or "One" and
    "Two" in X.md) across tracked docs and C comments."""
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


def _spelling(citation):
    return citation.value + "()" if citation.kind == "function" else citation.value


def _in_tree(root, citation, functions, macros):
    if citation.kind == "function":
        return citation.value in functions
    if citation.kind == "macro":
        return citation.value in macros
    return path_exists(root, citation.value, citation.doc)


def _outside(citation, outside):
    if citation.kind == "function":
        return citation.value in outside.functions
    if citation.kind == "macro":
        return citation.value in outside.constants or citation.value in outside.types
    return outside.has_path(citation.value)


def resolve(root, outside):
    """(missing, unchecked): the citations neither this tree nor `outside`,
    an idf_vocabulary.OutsideVocabulary, defines, and, when `outside` is
    None, no ESP-IDF to ask, the ones this tree does not define."""
    root = pathlib.Path(root)
    vocab = vocabulary(root)
    functions, macros = vocab.functions | vocab.script_functions, vocab.constants
    missing, unchecked = [], []
    for citation in citations(root):
        if citation.kind == "macro" and "_" not in citation.value:
            continue  # a shouted word, not a name
        if _in_tree(root, citation, functions, macros):
            continue
        if outside is None:
            unchecked.append(citation)
        elif not _outside(citation, outside):
            missing.append(citation)
    return missing, unchecked


def check(root, outside):
    return resolve(root, outside)[0]


def main(argv):
    root = pathlib.Path(".")
    require_idf = "--require-idf" in argv
    argv = [arg for arg in argv if arg != "--require-idf"]
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = pathlib.Path(argv[1])
    elif argv:
        print("usage: check_doc_citations.py [--root ROOT] [--require-idf]", file=sys.stderr)
        return 2
    outside = outside_vocabulary()
    if require_idf and required_missing(outside):
        print(required_missing(outside), file=sys.stderr)
        return 2
    try:
        missing, unchecked = resolve(root, outside)
        missing_sections = unresolved_sections(root)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    for item in missing:
        print(f"{item.doc}:{item.line}: missing {item.kind} citation {_spelling(item)}"
              f" (defined neither in this tree nor in ESP-IDF)")
    for citation, reason in missing_sections:
        print(f"{citation.doc}:{citation.line}: missing section citation - {reason}")
    if outside is None:
        print(not_verified_notice(len(unchecked)))
    print(f"{len(missing_sections)} missing section citation"
          f"{'' if len(missing_sections) == 1 else 's'}")
    print(f"{len(missing)} missing documentation citation"
          f"{'' if len(missing) == 1 else 's'}")
    return 1 if missing or missing_sections else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
