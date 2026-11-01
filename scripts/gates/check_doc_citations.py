#!/usr/bin/env python3
"""Fail when a documentation citation resolves neither in this tree nor in
ESP-IDF.

    python scripts/gates/check_doc_citations.py [--root ROOT] [--require-idf] [--fix]

A backticked function (`name()`), path or CONSTANT_NAME a doc cites must be
defined in this tree (the vendored components under launcher/components/
included) or, for what the firmware uses but does not define, declared
by ESP-IDF or its toolchain's C library (idf_vocabulary.outside_vocabulary()).
ESP-IDF counts for every chip it supports, so a name declared only for
another chip resolves too: that is the check's known limit. A quoted
section a doc or C comment cites must be a heading of the doc it
names. Under docs/plans/, which names what is not built yet, names are not
checked; section citations still are.

docs/Citations.md lists the papers and specs the tree cites, one table row
each, `| <a id="key"></a>[N] | ...<br>`name` cited by: ... |`. The key is the
entry's permanent name, lowercase words joined by hyphens that start with a
letter (`durstenfeld-1964`, `rfc4648`); a numeric anchor fails. [N] is the
row's position, shown only. A document cites by key and shows that number,
as `[[N]](path/Citations.md#` + key + `)` in a superscript, and every key it writes
must name an entry. Code and its comments never cite by anchor: a link into Citations.md in a
source file fails, and the comment names its source (author, year or section)
in place. An entry code uses opens its "cited by" line with that name in
backticks, written once, and the line must link exactly the documents that
cite it by key plus the source files that contain the name. The gates' own
tests write citations as fixtures, so they cite nothing. --fix rewrites every
shown number and every "Cited by" line to what the tree says.

Without an ESP-IDF checkout, a name this tree does not define cannot be
told apart from a typo, so it is counted, not failed, and one line says so.
--require-idf, which CI passes, makes a missing checkout, or a missing
toolchain C library beside it, an error instead.
"""
import pathlib
import posixpath
import re
import sys

from c_comments import SOURCE_EXTENSIONS, excluded, scan
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
CITATIONS = "docs/Citations.md"
CITED_BY = re.compile(r"<br>(?:`([^`\n]+)` cited by|Cited by): ([^|\n]*) \|$")
ROW_START = re.compile(r'^\| <a id="', re.MULTILINE)
CITATION_ROW = re.compile(
    r'^\| <a id="([^"\n]*)"></a>\[(\d+)\] \| [^|\n]+?' + CITED_BY.pattern, re.MULTILINE)
KEY = re.compile(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*")
PAPER = re.compile(r"Citations\.md#([\w-]+)")
SHOWN = re.compile(r"\[\[(\d+)\]\]\(([^)\s#]*Citations\.md)#([\w-]+)\)")
FIX_HINT = " (python scripts/gates/check_doc_citations.py --fix)"
CELL_LINK = re.compile(r"\]\(([^)\s#]+)\)")
CITING_DOCUMENTS = ["*.md"]
C_SOURCES = [f"*{suffix}" for suffix in SOURCE_EXTENSIONS]
CODE_SOURCES = [*C_SOURCES, "*.py", "*.sh", "*.mjs", "*.js", "*.cmake", "*CMakeLists.txt"]
GATE_TESTS = "scripts/gates/tests/"

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

    for rel in tracked_files(root, C_SOURCES):
        path = root / rel
        if not path.exists():
            continue
        if any(rel.startswith(e) for e in excluded()):
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


def citation_rows(root):
    """{key: (line, name, set of cited-by paths, position)} from
    docs/Citations.md, and the problems its entries have on their own. `name`
    is what code calls the entry, or None; `position` is its row number, the
    one a document shows. Cited-by links resolve relative to docs/."""
    root = pathlib.Path(root)
    rows, problems = {}, []
    path = root / CITATIONS
    if not path.is_file():
        return rows, problems
    text = path.read_text(encoding="utf-8")
    parsed = {m.start() for m in CITATION_ROW.finditer(text)}
    for start in ROW_START.finditer(text):
        if start.start() not in parsed:
            problems.append(f"{CITATIONS}:{text.count(chr(10), 0, start.start()) + 1}: entry does not end in "
                            "\"cited by: ...\" or \"`name` cited by: ...\"")
    for position, m in enumerate(CITATION_ROW.finditer(text), 1):
        number = text.count("\n", 0, m.start()) + 1
        key, shown, name, cited_line = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        if not KEY.fullmatch(key):
            problems.append(f"{CITATIONS}:{number}: anchor \"{key}\" is not a key: lowercase words joined by "
                            "hyphens, starting with a letter (author-year, rfc4648)")
        elif key in rows:
            problems.append(f"{CITATIONS}:{number}: key \"{key}\" is already the entry at line {rows[key][0]}")
        if shown != position:
            problems.append(f"{CITATIONS}:{number}: [{shown}] is row {position}{FIX_HINT}")
        cited_by = set()
        for target in CELL_LINK.findall(cited_line):
            resolved = (path.parent / target).resolve()
            try:
                cited_by.add(resolved.relative_to(root.resolve()).as_posix())
            except ValueError:
                problems.append(f"{CITATIONS}:{number}: cited-by link {target} leaves the tree")
        rows.setdefault(key, (number, name, cited_by, position))
    return rows, problems


def paper_citations(root):
    """{key: {citing path: first line}} for every Citations.md anchor the
    tracked documents write, outside Citations.md itself."""
    cited = {}
    for rel, text in citing_texts(root):
        for number, line in enumerate(text.splitlines(), 1):
            for key in PAPER.findall(line):
                cited.setdefault(key, {}).setdefault(rel, number)
    return cited


def citing_texts(root):
    """(relative path, text) for every tracked document that may cite a key,
    Citations.md and the gates' own tests aside."""
    root = pathlib.Path(root)
    for rel in sorted(tracked_files(root, CITING_DOCUMENTS)):
        if rel != CITATIONS and not rel.startswith(GATE_TESTS) and (root / rel).is_file():
            yield rel, (root / rel).read_text(encoding="utf-8", errors="replace")


def code_citations(root):
    """["path:line: ..."] for every Citations.md anchor a source file writes:
    code names its source in place, only documents cite by key."""
    root = pathlib.Path(root)
    found = []
    for rel in sorted(tracked_files(root, CODE_SOURCES)):
        if rel.startswith(GATE_TESTS) or not (root / rel).is_file():
            continue
        text = (root / rel).read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            if PAPER.search(line):
                found.append(f"{rel}:{number}: cites {CITATIONS} by anchor; "
                             "name the source (author, year, section) in place")
    return found


def named_in_code(root, rows):
    """{key: {source path: first line}} for every source file containing
    the name an entry carries, outside the gates' own tests."""
    root = pathlib.Path(root)
    names = {key: row[1] for key, row in rows.items() if row[1]}
    found = {}
    if not names:
        return found
    for rel in sorted(tracked_files(root, CODE_SOURCES)):
        if rel.startswith(GATE_TESTS) or not (root / rel).is_file():
            continue
        for number, line in enumerate((root / rel).read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for n, name in names.items():
                if name in line:
                    found.setdefault(n, {}).setdefault(rel, number)
    return found


def citers(root):
    """(rows, problems, papers, named, {key: sorted citing paths}): the
    documents citing an entry by key, the source files naming it, and both
    together."""
    rows, problems = citation_rows(root)
    papers = paper_citations(root)
    named = named_in_code(root, rows)
    together = {}
    for found in (papers, named):
        for key, files in found.items():
            together.setdefault(key, set()).update(files)
    return rows, problems, papers, named, {key: sorted(paths) for key, paths in together.items()}


def cited_by_cell(paths):
    """The "Cited by" cell for `paths`: links relative to docs/."""
    if not paths:
        return "none"
    return ", ".join(f"[{p}]({posixpath.relpath(p, posixpath.dirname(CITATIONS))})" for p in paths)


def unresolved_papers(root):
    """Every problem between docs/Citations.md and what cites it: a source file
    citing by anchor, a key with no entry (or a number in its place), a shown
    number that is not its entry's row, a cited-by line that differs from the
    citing documents plus the files naming the entry, a name no file carries,
    and a malformed or repeated key."""
    rows, problems, papers, named, cited = citers(root)
    problems += code_citations(root)
    for key, files in sorted(papers.items()):
        if key not in rows:
            hint = "; cite by its key, not its number" if key.isdigit() else ""
            for rel, line in sorted(files.items()):
                problems.append(f"{rel}:{line}: cites [{key}], which {CITATIONS} has no row for{hint}")
    problems += unresolved_numbers(root, rows)
    for n, (line, name, cited_by, _) in sorted(rows.items(), key=lambda item: item[1][0]):
        actual = set(cited.get(n, ()))
        if name and n not in named:
            problems.append(f"{CITATIONS}:{line}: [{n}] names `{name}` in code, "
                            "and no source file does; drop the name or the entry")
        if not actual and not name:
            problems.append(f"{CITATIONS}:{line}: [{n}] is cited nowhere")
        elif cited_by != actual:
            extra, absent = sorted(cited_by - actual), sorted(actual - cited_by)
            detail = "; ".join(filter(None, [
                absent and "cited by, not listed: " + ", ".join(absent),
                extra and "listed, does not cite it: " + ", ".join(extra)]))
            problems.append(f"{CITATIONS}:{line}: [{n}] {detail}{FIX_HINT}")
    return problems


def unresolved_numbers(root, rows):
    """Every citation a document writes as a shown number linked to its key
    (see the module docstring), N being key's row."""
    problems = []
    for rel, text in citing_texts(root):
        for number, line in enumerate(text.splitlines(), 1):
            spans = [m.span() for m in SHOWN.finditer(line)]
            for m in PAPER.finditer(line):
                if m.group(1) in rows and not any(a <= m.start() < b for a, b in spans):
                    problems.append(f"{rel}:{number}: cites [{m.group(1)}] without its shown number: "
                                    f"write [[N]](path/Citations.md#{m.group(1)})")
            for digits, _, key in (found.groups() for found in SHOWN.finditer(line)):
                if key in rows and int(digits) != rows[key][3]:
                    problems.append(f"{rel}:{number}: shows [{digits}] for {key}, "
                                    f"which is row {rows[key][3]}{FIX_HINT}")
    return problems


def shown_link(root, key, prefix="../"):
    """The superscript a generated document writes for key, the link `prefix` away."""
    rows, _ = citation_rows(root)
    if key not in rows:
        raise ValueError(f"{CITATIONS} has no row for {key}")
    return f"<sup>[[{rows[key][3]}]]({prefix}Citations.md#{key})</sup>"


def _rewrite(path, change):
    """Apply change(text) to the file, keeping its line endings; True if it changed."""
    with open(path, encoding="utf-8", newline="") as handle:
        raw = handle.read()
    eol = "\r\n" if "\r\n" in raw else "\n"
    text = raw.replace("\r\n", "\n")
    fixed = change(text)
    if fixed != text:
        path.write_bytes(fixed.replace("\n", eol).encode("utf-8"))
    return fixed != text


def fix_citations(root):
    """Rewrite docs/Citations.md so each row shows its position and each
    "Cited by" cell lists the citing documents plus the source files naming
    the entry, then every number a document shows to its entry's row; True if
    anything changed."""
    root = pathlib.Path(root)
    path = root / CITATIONS
    if not path.is_file():
        return False
    cited = citers(root)[-1]
    positions = iter(range(1, len(CITATION_ROW.findall(path.read_text(encoding="utf-8"))) + 1))

    def row(m):
        head = m.group(0)[:m.start(4) - m.start()].replace(f"[{m.group(2)}] |", f"[{next(positions)}] |", 1)
        return head + cited_by_cell(cited.get(m.group(1), [])) + " |"

    changed = _rewrite(path, lambda text: CITATION_ROW.sub(row, text))
    rows, _ = citation_rows(root)

    def shown(m):
        found = rows.get(m.group(3))
        return f"[[{found[3]}]]({m.group(2)}#{m.group(3)})" if found else m.group(0)

    for rel, _ in citing_texts(root):
        changed |= _rewrite(root / rel, lambda text: SHOWN.sub(shown, text))
    return changed


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
    fix = "--fix" in argv
    argv = [arg for arg in argv if arg not in ("--require-idf", "--fix")]
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = pathlib.Path(argv[1])
    elif argv:
        print("usage: check_doc_citations.py [--root ROOT] [--require-idf] [--fix]", file=sys.stderr)
        return 2
    if fix:
        print("rewrote shown numbers and Cited by lines" if fix_citations(root) else "shown numbers and Cited by lines already match")
    outside = outside_vocabulary()
    if require_idf and required_missing(outside):
        print(required_missing(outside), file=sys.stderr)
        return 2
    try:
        missing, unchecked = resolve(root, outside)
        missing_sections = unresolved_sections(root)
        paper_problems = unresolved_papers(root)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    for item in missing:
        print(f"{item.doc}:{item.line}: missing {item.kind} citation {_spelling(item)}"
              f" (defined neither in this tree nor in ESP-IDF)")
    for citation, reason in missing_sections:
        print(f"{citation.doc}:{citation.line}: missing section citation - {reason}")
    for problem in paper_problems:
        print(problem)
    if outside is None:
        print(not_verified_notice(len(unchecked)))
    print(f"{len(missing_sections)} missing section citation"
          f"{'' if len(missing_sections) == 1 else 's'}")
    print(f"{len(missing)} missing documentation citation"
          f"{'' if len(missing) == 1 else 's'}")
    print(f"{len(paper_problems)} paper citation problem"
          f"{'' if len(paper_problems) == 1 else 's'}")
    return 1 if missing or missing_sections or paper_problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
