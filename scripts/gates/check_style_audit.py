#!/usr/bin/env python3
"""Style-audit gate: decidable rules from a whole-repo review pass that no
other scripts/gates/*.py already enforces.

    python scripts/gates/check_style_audit.py [--root ROOT]

One function per rule, each with a short id and a one-line message; findings
print as `path:line: [RULE-ID] message`. A rule that would need a human
judgement call on real content here was left out rather than scripted badly -
see the PR that added this file for the list and why.
"""
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_comment_length import EXCLUDED, scan  # noqa: E402

SKIP_DIRS = ("build", "build.dev", "build.diag", "build.qemu", "build.qemu.perf",
             "build.qemu.shell", "managed_components", "components")

BINARY_EXTENSIONS = {".png", ".jpg", ".jpeg", ".gif", ".ico", ".bmp", ".otf", ".ttf",
                     ".woff", ".woff2", ".xcf", ".bin", ".elf", ".o", ".a", ".exe",
                     ".dll", ".pyc", ".gz", ".zip", ".wav", ".mp3", ".mp4", ".pdf"}


class Finding:
    def __init__(self, path, line, rule, message):
        self.path = path
        self.line = line
        self.rule = rule
        self.message = message

    def __str__(self):
        return f"{self.path}:{self.line}: [{self.rule}] {self.message}"


def tracked_files(root, patterns):
    result = subprocess.run(["git", "ls-files", *patterns], cwd=root,
                            capture_output=True, text=True, check=True)
    return [line for line in result.stdout.splitlines() if line]


def c_files(root):
    """First-party .c/.h files: vendored trees and GENERATED FILE headers
    keep their upstream or generator-owned form, the same exemption
    check-format.sh and check_comment_length.py already give them - see
    scripts/gates/format-file-list.sh's own comment for why the marker,
    not a path list, is what decides "generated"."""
    root = pathlib.Path(root)
    for rel in tracked_files(root, ["*.c", "*.h"]):
        if any(part in SKIP_DIRS for part in pathlib.PurePosixPath(rel).parts):
            continue
        if any(rel.startswith(e) for e in EXCLUDED):
            continue
        path = root / rel
        if not path.is_file():
            continue
        head = "\n".join(path.read_text(encoding="utf-8", errors="replace").splitlines()[:5])
        if "GENERATED FILE" in head:
            continue
        yield rel, path


def markdown_files(root):
    """Every tracked document under docs/, plus a top-level *.md - the same
    set check_doc_citations.documentation() reads, so this gate and that one
    never disagree about what counts as documentation."""
    root = pathlib.Path(root)
    yield from sorted((root / "docs").rglob("*.md"))
    for rel in tracked_files(root, ["*.md"]):
        if "/" not in rel:
            yield root / rel


def relpath(root, path):
    return pathlib.Path(path).resolve().relative_to(pathlib.Path(root).resolve()).as_posix()


def strip_fences(lines):
    """Yield (line_number, line) skipping the body of fenced code blocks -
    a rule about prose has no business reading a shell transcript."""
    fenced = False
    for number, line in enumerate(lines, 1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        yield number, line


# --------------------------------------------------------------------------
# RULE: tracker issue ids and git commit hashes never belong in tracked text
# - they name a system this repo does not keep in sync with itself. A bd/
# beads id is always written "bd autana-<code>" in this tree (see
# .beads/PRIME.md); a bare "autana-cli"-shaped compound word is not one. A
# full 40-character SHA is unambiguous on its own; a short hash is not (see
# suite_sand_perf.c's own build-id comments, which are legitimate perf
# provenance, not a tracker reference) so only the full form is flagged.

TRACKER_ID = re.compile(r"\bbd\s+(?:show\s+|graph\s+)?autana-[a-zA-Z0-9]+\b")
COMMIT_SHA = re.compile(r"\b[0-9a-f]{40}\b")


def rule_tracker_and_commit_refs(root):
    findings = []
    for rel, path in c_files(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        for c in scan(rel, text):
            for pattern, what in ((TRACKER_ID, "a tracker issue id"), (COMMIT_SHA, "a git commit hash")):
                m = pattern.search(c.text)
                if m:
                    findings.append(Finding(rel, c.line, "TRACKER-REF",
                                            f"comment names {what} ({m.group(0)}) - issue ids and commit "
                                            "hashes never go into tracked text"))
    for path in markdown_files(root):
        rel = relpath(root, path)
        for number, line in strip_fences(path.read_text(encoding="utf-8", errors="replace").splitlines()):
            for pattern, what in ((TRACKER_ID, "a tracker issue id"), (COMMIT_SHA, "a git commit hash")):
                m = pattern.search(line)
                if m:
                    findings.append(Finding(rel, number, "TRACKER-REF",
                                            f"names {what} ({m.group(0)}) - issue ids and commit hashes "
                                            "never go into tracked text"))
    return findings


# --------------------------------------------------------------------------
# RULE: a calendar date in a C comment or a doc records when something was
# true rather than what is true now - the journey, not the constraint. A
# genuinely dated log (docs/doc_review_ledger.txt: path, date, note, one row
# per review) is exactly what a date belongs in, and it is a .txt outside
# this rule's .md/.c/.h scope - the exception the task asked to look for.

DATE = re.compile(r"\b(?:19|20)\d{2}-\d{2}-\d{2}\b")


def rule_calendar_dates(root):
    findings = []
    for rel, path in c_files(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        for c in scan(rel, text):
            m = DATE.search(c.text)
            if m:
                findings.append(Finding(rel, c.line, "CALENDAR-DATE",
                                        f"comment carries a calendar date ({m.group(0)}) - "
                                        "state the constraint, not when it was measured"))
    for path in markdown_files(root):
        rel = relpath(root, path)
        for number, line in strip_fences(path.read_text(encoding="utf-8", errors="replace").splitlines()):
            m = DATE.search(line)
            if m:
                findings.append(Finding(rel, number, "CALENDAR-DATE",
                                        f"carries a calendar date ({m.group(0)}) - "
                                        "state the constraint, not when it was measured"))
    return findings


# --------------------------------------------------------------------------
# RULE: "living document" tells a reader nothing a git log doesn't already
# say more precisely, and CLAUDE.md's own "documents state current reality"
# rule makes the phrase redundant by construction.

LIVING_DOCUMENT = re.compile(r"living document", re.I)


def rule_living_document(root):
    findings = []
    for path in markdown_files(root):
        rel = relpath(root, path)
        for number, line in strip_fences(path.read_text(encoding="utf-8", errors="replace").splitlines()):
            if LIVING_DOCUMENT.search(line):
                findings.append(Finding(rel, number, "LIVING-DOC",
                                        "\"living document\" - say what the doc covers, not that it changes"))
    return findings


# --------------------------------------------------------------------------
# RULE: includes are layer-qualified (docs/C-Style-Guide.md, "Headers and
# module boundaries"): "gfx/gfx.h", never "../../gfx/gfx.h" or a bare
# "gfx.h", even between two files in the same folder. launcher/main is
# registered as an INCLUDE_DIRS root ("." in launcher/main/CMakeLists.txt),
# so every header under one of the shared layers, or living at main/'s own
# root, is reachable without a single dot. Scoped to those layers only - an
# app's own internal includes (material.h from a sibling file in the same
# app) are not what CLAUDE.md's "an app reaching past ui into gfx" is about,
# and apps are free to organise their own folder however they like.

INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"')
LAYER_DIRS = ("boot", "display", "gfx", "input", "render", "ui", "util", "console")
LAYER_ROOT_FILES = ("app.h", "build_variant.h")


def _layer_headers(main_dir):
    headers = {}
    for layer in LAYER_DIRS:
        for p in (main_dir / layer).rglob("*.h"):
            headers.setdefault(p.name, []).append(f"{layer}/{p.relative_to(main_dir / layer).as_posix()}")
    return headers


def rule_include_layering(root):
    main_dir = pathlib.Path(root) / "launcher/main"
    if not main_dir.is_dir():
        return []
    layer_headers = _layer_headers(main_dir)
    findings = []
    for rel, path in c_files(root):
        if not rel.startswith("launcher/main/"):
            continue
        under_main = rel[len("launcher/main/"):]
        if under_main.startswith("apps/") and "/tools/" in under_main:
            continue  # apps/*/tools/ is excluded from the firmware glob build entirely
        text = path.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            m = INCLUDE.match(line)
            if not m:
                continue
            inc = m.group(1)
            if "/" not in inc and inc in layer_headers:
                findings.append(Finding(rel, number, "INCLUDE-LAYER",
                                        f'"{inc}" names a layer header without its folder - use '
                                        f'"{layer_headers[inc][0]}"'))
                continue
            if "../" in inc:
                resolved = (path.parent / inc).resolve()
                try:
                    rel_to_main = resolved.relative_to(main_dir.resolve())
                except ValueError:
                    continue
                top = rel_to_main.as_posix().split("/")[0]
                if resolved.exists() and (top in LAYER_DIRS or rel_to_main.as_posix() in LAYER_ROOT_FILES):
                    findings.append(Finding(rel, number, "INCLUDE-LAYER",
                                            f'"{inc}" is relative - use "{rel_to_main.as_posix()}"'))
    return findings


# --------------------------------------------------------------------------
# RULE: a personal home-directory path baked into tracked source only works
# on the machine that wrote it. `~`/os.path.expanduser and a version glob
# (see launcher/test/qemu_run.py's own find_one() call) are the portable
# form already established in this tree.

PERSONAL_PATH = re.compile(
    r"C:\\Users\\[A-Za-z0-9][A-Za-z0-9_.-]*|/home/[A-Za-z0-9][A-Za-z0-9_.-]*|/Users/[A-Za-z0-9][A-Za-z0-9_.-]*")


def rule_personal_paths(root):
    root = pathlib.Path(root)
    findings = []
    for rel in tracked_files(root, []):
        p = pathlib.Path(rel)
        if p.suffix.lower() in BINARY_EXTENSIONS or any(part in SKIP_DIRS for part in p.parts):
            continue
        full = root / rel
        if not full.is_file():
            continue
        try:
            text = full.read_text(encoding="utf-8", errors="strict")
        except (UnicodeDecodeError, OSError):
            continue
        for number, line in enumerate(text.splitlines(), 1):
            m = PERSONAL_PATH.search(line)
            if m:
                findings.append(Finding(rel, number, "PERSONAL-PATH",
                                        f"personal path {m.group(0)} - only works on the machine that wrote it"))
    return findings


# --------------------------------------------------------------------------
# RULE: a markdown anchor link must resolve to a heading GitHub will
# actually generate that id for. GitHub's slugger lowercases, drops any
# character that is not a letter, digit, space, or hyphen, then turns each
# remaining space into its own hyphen - critically, it does not collapse a
# run of spaces into one hyphen, so a heading punctuated with an em dash
# ("SD card <2014> fully...") slugs with a double hyphen, and this has to
# match that or every such heading in this tree would falsely fail.

LINK = re.compile(r"\]\(([^)\s]+)\)")
HEADING = re.compile(r"^(#{1,6})\s+(.+?)\s*#*$")
INLINE_MARKUP = (
    (re.compile(r"`([^`]*)`"), r"\1"),
    (re.compile(r"\*\*([^*]*)\*\*"), r"\1"),
    (re.compile(r"\*([^*]*)\*"), r"\1"),
    (re.compile(r"\[([^\]]*)\]\([^)]*\)"), r"\1"),
)


def slugify(text):
    for pattern, repl in INLINE_MARKUP:
        text = pattern.sub(repl, text)
    text = text.strip().lower()
    text = re.sub(r"[^\w\s-]", "", text)
    return text.replace(" ", "-")


def heading_slugs(path):
    if not path.is_file():
        return None
    slugs, counts = set(), {}
    for _, line in strip_fences(path.read_text(encoding="utf-8", errors="replace").splitlines()):
        m = HEADING.match(line)
        if not m:
            continue
        slug = slugify(m.group(2))
        n = counts.get(slug, 0)
        counts[slug] = n + 1
        slugs.add(slug if n == 0 else f"{slug}-{n}")
    return slugs


def rule_anchor_links(root):
    root = pathlib.Path(root)
    findings = []
    cache = {}
    for path in markdown_files(root):
        rel = relpath(root, path)
        for number, line in strip_fences(path.read_text(encoding="utf-8", errors="replace").splitlines()):
            for target in LINK.findall(line):
                if "://" in target or target.startswith("mailto:"):
                    continue
                file_part, sep, anchor = target.partition("#")
                if not sep or not anchor:
                    continue
                if file_part:
                    target_path = (path.parent / file_part).resolve()
                else:
                    target_path = path.resolve()
                key = str(target_path)
                if key not in cache:
                    cache[key] = heading_slugs(target_path)
                slugs = cache[key]
                if slugs is None:
                    findings.append(Finding(rel, number, "ANCHOR-LINK",
                                            f"({target}) points at a file that does not exist"))
                elif anchor not in slugs:
                    findings.append(Finding(rel, number, "ANCHOR-LINK",
                                            f"#{anchor} matches no heading in "
                                            f"{file_part or pathlib.Path(rel).name}"))
    return findings


# --------------------------------------------------------------------------
# RULE: a stray HTML comment in prose is either an editor's leftover markup
# or a hand-written aside nobody will see rendered - Markdown's own asterisk
# emphasis and backtick code say the same things visibly. The gates already
# use exactly two escape markers (doc-vocabulary, doc-constants) and one
# generator marks its output with BEGIN/END GENERATED; anything else is a
# stray.

HTML_COMMENT = re.compile(r"<!--.*?-->")
KNOWN_MARKERS = re.compile(r"doc-(?:vocabulary|constants|citation)\s*:\s*ignore|(?:BEGIN|END)\s+GENERATED", re.I)


def rule_stray_html_comments(root):
    findings = []
    for path in markdown_files(root):
        rel = relpath(root, path)
        for number, line in strip_fences(path.read_text(encoding="utf-8", errors="replace").splitlines()):
            for m in HTML_COMMENT.finditer(line):
                if not KNOWN_MARKERS.search(m.group(0)):
                    findings.append(Finding(rel, number, "STRAY-HTML-COMMENT",
                                            f"{m.group(0)[:60]} - Markdown has no rendered use for this"))
    return findings


# --------------------------------------------------------------------------
# RULE: a doc or comment can cite a docs/*.md path, or a quoted section name
# in one, that check_doc_citations.py does not check - its own FILE regex
# only recognises .c/.h/.py/.sh/.cmake and CMakeLists.txt, never .md. A
# quoted section is allowed to be a shorthand prefix of the real heading
# ("Sand-Simulation.md's "Two cores" section" for the heading "Two cores:
# chunk-parallel passes, and what stays serial") - only a name matching no
# heading at all is stale.

DOC_PATH = re.compile(r"`(docs/[A-Za-z0-9_./-]+\.md)`")
SECTION_IN_DOC = re.compile(r'"([^"\n]{2,80})"\s+in\s+([A-Za-z0-9_./-]+\.md)')
DOC_SECTION = re.compile(r"([A-Za-z0-9_./-]+\.md)'s\s+\"([^\"\n]{2,80})\"")


def _doc_headings(root, doc_ref, relative_to):
    path = (pathlib.Path(root) / doc_ref) if doc_ref.startswith("docs/") else (relative_to.parent / doc_ref)
    if not path.is_file():
        return None
    heads = set()
    for _, line in strip_fences(path.read_text(encoding="utf-8", errors="replace").splitlines()):
        m = re.match(r"^#{1,6}\s+(.+?)\s*#*$", line)
        if m:
            heads.add(m.group(1).strip())
    return heads


def _section_missing(section, heads):
    return heads is not None and not any(h == section or h.startswith(section) for h in heads)


def rule_doc_citations(root):
    root = pathlib.Path(root)
    findings = []
    for path in markdown_files(root):
        rel = relpath(root, path)
        for number, line in strip_fences(path.read_text(encoding="utf-8", errors="replace").splitlines()):
            for m in DOC_PATH.finditer(line):
                if not (root / m.group(1)).is_file():
                    findings.append(Finding(rel, number, "DOC-CITATION",
                                            f"cites {m.group(1)}, which does not exist"))
            for m in SECTION_IN_DOC.finditer(line):
                section, doc = m.group(1), m.group(2)
                heads = _doc_headings(root, doc, path)
                if _section_missing(section, heads):
                    findings.append(Finding(rel, number, "DOC-SECTION",
                                            f'"{section}" is not a heading in {doc}'))
            for m in DOC_SECTION.finditer(line):
                doc, section = m.group(1), m.group(2)
                heads = _doc_headings(root, doc, path)
                if _section_missing(section, heads):
                    findings.append(Finding(rel, number, "DOC-SECTION",
                                            f'"{section}" is not a heading in {doc}'))
    for rel, path in c_files(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        for c in scan(rel, text):
            for m in DOC_PATH.finditer(c.text):
                if not (root / m.group(1)).is_file():
                    findings.append(Finding(rel, c.line, "DOC-CITATION",
                                            f"comment cites {m.group(1)}, which does not exist"))
    return findings


# --------------------------------------------------------------------------
# RULE: a line starting "- " renders as a list item whether the author meant
# one or not. Flag it only when the previous line is itself unindented,
# ordinary top-level prose - not blank, not a heading/quote/table row, not
# itself a list item, and not ending in ":" (which legitimately introduces
# one) - since an indented line is prose continuing an existing list item,
# not a paragraph an editor's wrap coincidentally broke before a "-".

BULLET = re.compile(r"^- \S")
LIST_MARKER = re.compile(r"^\s*([-*+]|\d+\.)\s")


def rule_accidental_bullets(root):
    findings = []
    for path in markdown_files(root):
        rel = relpath(root, path)
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        fenced = False
        for i, line in enumerate(lines):
            if line.lstrip().startswith("```"):
                fenced = not fenced
                continue
            if fenced or i == 0 or not BULLET.match(line):
                continue
            prev = lines[i - 1]
            if prev.strip() == "" or prev != prev.lstrip():
                continue
            if LIST_MARKER.match(prev) or prev.lstrip().startswith(("#", ">", "|")):
                continue
            if prev.rstrip().endswith(":"):
                continue
            findings.append(Finding(rel, i + 1, "ACCIDENTAL-BULLET",
                                    'starts "- " right after unindented prose - renders as a list item; '
                                    "rewrap the paragraph"))
    return findings


# --------------------------------------------------------------------------
# RULE: a one-line Title Case comment with no sentence, sitting inside a
# function body, is a leftover section label for a block that should have
# been an extracted, named helper instead - docs/C-Style-Guide.md's "needing
# to comment parts of a function separately means the function is the
# problem." The identical shape is also this tree's normal way to divide a
# FILE's top-level declarations into named groups (e.g. gfx.c's own
# "/* Dirty tracking */" before its dirty-tracking statics) - that is a
# different, accepted use, so this only fires once brace-depth tracking
# proves the comment sits inside a real function body, not a top-level
# initializer, struct, or a file-level divider between declarations.

LABEL = re.compile(r"^[A-Z][a-z]+(?:\s[A-Za-z][a-z]*){0,4}$")


def _function_body_states(source, comments):
    """{id(comment): bool}: whether each comment sits inside a function
    body. A '{' at brace-depth 0 opens a function body only when the last
    non-space character before it is ')' - the one shape a struct/array/enum
    initializer's '{' never has, since those close on '=' or a bare type
    keyword. Everything nested inside a function-body frame - if/for/switch
    blocks, compound literals - inherits that frame's state, matching how
    the rule cares about the whole function, not just its outermost brace."""
    spans = {}
    for c in comments:
        for start, _ in c.spans:
            spans[start] = c

    states = {}
    stack = []
    i, n = 0, len(source)
    last_nonspace = ""
    while i < n:
        if i in spans:
            c = spans[i]
            states[id(c)] = stack[-1] if stack else False
        ch = source[i]
        if ch in "\"'":
            quote = ch
            i += 1
            while i < n and source[i] != quote:
                i += 2 if source[i] == "\\" and i + 1 < n else 1
            i += 1
            last_nonspace = quote
            continue
        if source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        if source.startswith("//", i):
            end = source.find("\n", i)
            i = n if end < 0 else end
            continue
        if ch == "{":
            is_fn = stack[-1] if stack else (last_nonspace == ")")
            stack.append(is_fn)
        elif ch == "}":
            if stack:
                stack.pop()
        if not ch.isspace():
            last_nonspace = ch
        i += 1
    return states


def rule_heading_comments_in_functions(root):
    findings = []
    for rel, path in c_files(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        comments = scan(rel, text)
        states = _function_body_states(text, comments)
        for c in comments:
            if c.is_banner or c.kind != "block" or c.lines != 1:
                continue
            t = c.text.strip()
            if LABEL.match(t) and states.get(id(c)):
                findings.append(Finding(rel, c.line, "HEADING-COMMENT",
                                        f'"{t}" is a section label inside a function body - '
                                        "extract a named helper instead"))
    return findings


RULES = (
    ("TRACKER-REF", rule_tracker_and_commit_refs),
    ("CALENDAR-DATE", rule_calendar_dates),
    ("LIVING-DOC", rule_living_document),
    ("INCLUDE-LAYER", rule_include_layering),
    ("PERSONAL-PATH", rule_personal_paths),
    ("ANCHOR-LINK", rule_anchor_links),
    ("STRAY-HTML-COMMENT", rule_stray_html_comments),
    ("DOC-CITATION", rule_doc_citations),
    ("ACCIDENTAL-BULLET", rule_accidental_bullets),
    ("HEADING-COMMENT", rule_heading_comments_in_functions),
)


def check(root="."):
    findings = []
    for _, rule in RULES:
        findings.extend(rule(root))
    findings.sort(key=lambda f: (f.path, f.line))
    return findings


def main(argv):
    root = "."
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = argv[1]
    elif argv:
        print("usage: check_style_audit.py [--root ROOT]", file=sys.stderr)
        return 2
    findings = check(root)
    for f in findings:
        print(f)
    print(f"{len(findings)} style-audit finding{'' if len(findings) == 1 else 's'}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
