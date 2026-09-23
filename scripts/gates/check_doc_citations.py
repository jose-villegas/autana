#!/usr/bin/env python3
"""Fail when a documentation citation no longer resolves in this tree.

    python scripts/gates/check_doc_citations.py [--root ROOT]
"""
import pathlib
import re
import subprocess
import sys

from check_comment_length import EXCLUDED as C_EXCLUDED, scan
from check_doc_index import blank_fences, doc_headings
from code_vocabulary import names

INLINE = re.compile(r"`([^`\n]+)`")
FUNCTION = re.compile(r"^([a-z][a-z0-9_]*)\(\)$")
MACRO = re.compile(r"^[A-Z][A-Z0-9_]*$")
FILE = re.compile(r"^(?:launcher/|apps/|[\w.-]+/)*(?:[\w.-]+\.(?:c|h|py|sh|cmake|md)|CMakeLists\.txt)$")
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
    def __init__(self, doc, line, kind, value):
        self.doc = doc
        self.line = line
        self.kind = kind
        self.value = value


def tree_files(root):
    """What a citation may resolve to: the tracked files, since a build tree
    or a checkout nested inside this one is not what CI sees. Outside git (a
    test fixture), every file under root."""
    root = pathlib.Path(root)
    result = subprocess.run(["git", "ls-files"], cwd=root, capture_output=True, text=True)
    if result.returncode:
        return sorted(path for path in root.rglob("*") if path.is_file())
    return [root / name for name in sorted(result.stdout.splitlines())]


def documentation(root):
    root = pathlib.Path(root)
    yield from sorted((root / "docs").rglob("*.md"))
    result = subprocess.run(["git", "ls-files", "*.md"], cwd=root,
                            capture_output=True, text=True)
    if result.returncode:
        yield from sorted(root.glob("*.md"))
        return
    for name in sorted(result.stdout.splitlines()):
        if "/" not in name:
            yield root / name


def citations(root):
    root = pathlib.Path(root)
    for path in documentation(root):
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
            if skip:
                continue
            for text in INLINE.findall(line):
                function = FUNCTION.fullmatch(text)
                if function:
                    yield Citation(path.relative_to(root).as_posix(), number,
                                   "function", function.group(1))
                elif FILE.fullmatch(text):
                    yield Citation(path.relative_to(root).as_posix(), number,
                                   "path", text)
                elif MACRO.fullmatch(text):
                    yield Citation(path.relative_to(root).as_posix(), number,
                                   "macro", text)


ALLOWLIST = "scripts/gates/doc_citation_allowlist.txt"
PLANS = ("docs/plans/*", "*")


def allowlist(root):
    """(doc, citation) -> allowlist line number."""
    path = pathlib.Path(root) / ALLOWLIST
    allowed = {}
    if not path.exists():
        return allowed
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or not all(fields):
            raise ValueError(f"{path}:{number}: expected doc, citation, reason")
        allowed[(fields[0], fields[1])] = number
    return allowed


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
        for path in tree_files(root):
            if path.as_posix().endswith("/" + value) and path.exists():
                return path
        return None
    for path in tree_files(root):
        if path.name == value and path.exists():
            return path
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

    for path in tree_files(root):
        rel = path.relative_to(root).as_posix()
        if not rel.startswith("launcher/") or path.suffix not in (".c", ".h") or not path.exists():
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


def allowlist_entry(allowed, citation):
    if (citation.doc, citation.value) in allowed:
        return (citation.doc, citation.value)
    if PLANS in allowed and citation.doc.startswith("docs/plans/"):
        return PLANS
    return None


def unresolved(root):
    """Every citation that does not resolve, allowlisted or not."""
    root = pathlib.Path(root)
    functions, macros = names(root)
    missing = []
    for citation in citations(root):
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
    allowed = allowlist(root)
    return [citation for citation in unresolved(root)
            if allowlist_entry(allowed, citation) is None]


def stale_allowlist(root):
    """Allowlist entries that no unresolved citation needs, as (line, doc, citation)."""
    allowed = allowlist(root)
    used = {allowlist_entry(allowed, citation) for citation in unresolved(root)}
    return sorted((number, *entry) for entry, number in allowed.items() if entry not in used)


def main(argv):
    root = pathlib.Path(".")
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = pathlib.Path(argv[1])
    elif argv:
        print("usage: check_doc_citations.py [--root ROOT]", file=sys.stderr)
        return 2
    try:
        missing = check(root)
        stale = stale_allowlist(root)
        missing_sections = unresolved_sections(root)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    for item in missing:
        label = item.value + "()" if item.kind == "function" else item.value
        print(f"{item.doc}:{item.line}: missing {item.kind} citation {label}")
    for citation, reason in missing_sections:
        print(f"{citation.doc}:{citation.line}: missing section citation - {reason}")
    for number, doc, citation in stale:
        print(f"{ALLOWLIST}:{number}: stale entry {doc} {citation}: nothing left to allow")
    print(f"{len(missing_sections)} missing section citation"
          f"{'' if len(missing_sections) == 1 else 's'}")
    print(f"{len(missing)} missing documentation citation"
          f"{'' if len(missing) == 1 else 's'}, {len(stale)} stale allowlist "
          f"entr{'y' if len(stale) == 1 else 'ies'}")
    return 1 if missing or stale or missing_sections else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
