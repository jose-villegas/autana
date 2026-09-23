#!/usr/bin/env python3
"""Decidable prose and include rules no other scripts/gates/*.py enforces.

    python scripts/gates/check_style_audit.py [--fix] [--strict] [--rule ID] [--file TEXT]

One function per rule, registered by decorator (@doc_rule, @c_comment_rule,
@c_line_rule, @text_rule) with an id, a severity, and an optional fixer. A
finding prints as `path:line: [SEVERITY RULE-ID] message`. ERROR fails the
run; WARN prints but only fails under --strict. `--fix` applies every rule's
fixer, then re-audits, so the exit code reflects what actually remains.
`--rule` runs one rule; `--file` scopes to files whose path contains the
given text and exits 2 (never a false "clean") if that matches nothing.

Two rules a script cannot judge reliably were left out rather than scripted
badly: journey words in prose docs (find_narrative_comments.py's own
SIGNS list is, by its own docstring, "a WORKLIST, NOT A VERDICT" - tested
against this tree's docs it is nearly all ordinary technical prose, not
narration) and whether a comment should exist at all (docs/C-Style-Guide.md
calls that judgement, not a length or a name). HEADING-COMMENT is WARN for
the same reason - a worklist, not a verdict.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_comment_length import EXCLUDED, scan  # noqa: E402
from check_doc_citations import documentation  # noqa: E402
from check_doc_constants import ESCAPE as DOC_CONSTANTS_ESCAPE  # noqa: E402
from check_doc_index import blank_fences  # noqa: E402
from check_doc_vocabulary import ESCAPE as DOC_VOCABULARY_ESCAPE  # noqa: E402
import strip_comment_rules  # noqa: E402
from tracked import tracked_files  # noqa: E402

ERROR, WARN = "ERROR", "WARN"

BINARY_EXTENSIONS = {".png", ".jpg", ".jpeg", ".gif", ".ico", ".bmp", ".otf", ".ttf",
                     ".woff", ".woff2", ".xcf", ".bin", ".elf", ".o", ".a", ".exe",
                     ".dll", ".pyc", ".gz", ".zip", ".wav", ".mp3", ".mp4", ".pdf"}


class Finding:
    def __init__(self, path, line, rule_id, severity, message, fixable):
        self.path = path
        self.line = line
        self.rule_id = rule_id
        self.severity = severity
        self.message = message
        self.fixable = fixable

    def __str__(self):
        tag = f"[{self.severity} {self.rule_id}]" + (" (--fix)" if self.fixable else "")
        return f"{self.path}:{self.line}: {tag} {self.message}"


class Rule:
    def __init__(self, rule_id, severity, kind, func, fixer):
        self.id = rule_id
        self.severity = severity
        self.kind = kind
        self.func = func
        self.fixer = fixer


RULES = []


def _register(kind):
    def make_decorator(rule_id, severity=ERROR, fixer=None):
        def decorator(func):
            RULES.append(Rule(rule_id, severity, kind, func, fixer))
            return func
        return decorator
    return make_decorator


# Every rule function takes (root, path, data) - or, for a c_comment_rule,
# (root, path, text, comments), since a comment rule sometimes needs the raw
# text too (brace context, say) and the walker already read it - and yields
# (line, message) pairs. Every fixer takes (root, path, text) and returns
# the file's new text, or None to leave it alone - the same shape
# regardless of which walker found the violation, so --fix does not need to
# know which kind of rule it is re-running.
doc_rule = _register("doc")
c_comment_rule = _register("c_comment")
c_line_rule = _register("c_line")
text_rule = _register("text")


def relpath(root, path):
    return pathlib.Path(path).resolve().relative_to(pathlib.Path(root).resolve()).as_posix()


# Walkers: each reads a file exactly once and hands every registered rule of
# its kind the same parsed data.

def _doc_walk(root):
    for path in documentation(root):
        raw = path.read_text(encoding="utf-8", errors="replace").splitlines()
        yield path, raw


def _c_walk(root):
    """First-party .c/.h files: vendored trees and GENERATED FILE headers
    keep their upstream or generator-owned form - the same exemption
    check-format.sh and check_comment_length.py give them."""
    root = pathlib.Path(root)
    for rel in tracked_files(root, ["*.c", "*.h"]):
        if any(rel.startswith(e) for e in EXCLUDED):
            continue
        path = root / rel
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "GENERATED FILE" in "\n".join(text.splitlines()[:5]):
            continue
        yield path, text


def _text_walk(root):
    root = pathlib.Path(root)
    for rel in tracked_files(root, []):
        p = pathlib.Path(rel)
        if p.suffix.lower() in BINARY_EXTENSIONS:
            continue
        full = root / rel
        if not full.is_file():
            continue
        try:
            text = full.read_text(encoding="utf-8", errors="strict")
        except (UnicodeDecodeError, OSError):
            continue
        yield full, text


# RULE: a tracker issue id or a git commit hash names a system this repo
# does not keep in sync with itself - it belongs in bd/beads or git, never
# in tracked text. A bd id is a short, 3-4 character code (autana-zq7,
# autana-qqx); "bd " in front is optional. The only real compound word this
# repo's own CLI names that shape - "autana-cli" - is excluded by name,
# since every other one ("autana-screenshot", "autana-device",
# "autana-monitor") is already too long to match. A 40-character hex run is
# an unambiguous full SHA; a 12-character build id is perf provenance, not
# a tracker reference.

TRACKER_ID = re.compile(
    r"\bbd\s+(?:show\s+|graph\s+)?autana-[a-zA-Z0-9]+\b|\bautana-(?!cli\b)[a-z0-9]{3,4}\b")
COMMIT_SHA = re.compile(r"\b[0-9a-f]{40}\b")


def _tracker_refs(text):
    for pattern, what in ((TRACKER_ID, "a tracker issue id"), (COMMIT_SHA, "a git commit hash")):
        m = pattern.search(text)
        if m:
            yield what, m.group(0)


TRACKER_REF_ACTION = "move it to the PR body or bd and state the fact itself"


@doc_rule("TRACKER-REF")
def rule_tracker_ref_doc(root, path, raw_lines):
    for number, line in enumerate(blank_fences(raw_lines), 1):
        for what, value in _tracker_refs(line):
            yield number, f"names {what} ({value}) - {TRACKER_REF_ACTION}"


@c_comment_rule("TRACKER-REF")
def rule_tracker_ref_comment(root, path, text, comments):
    for c in comments:
        for what, value in _tracker_refs(c.text):
            yield c.line, f"comment names {what} ({value}) - {TRACKER_REF_ACTION}"


# RULE: a calendar date in a C comment or a doc records when something was
# true, not what is true now. docs/doc_review_ledger.txt is a genuine dated
# log; as a .txt it is outside this rule's .md/.c/.h scope.

DATE = re.compile(r"\b(?:19|20)\d{2}-\d{2}-\d{2}\b")


@doc_rule("CALENDAR-DATE")
def rule_calendar_date_doc(root, path, raw_lines):
    for number, line in enumerate(blank_fences(raw_lines), 1):
        m = DATE.search(line)
        if m:
            yield number, f"carries a calendar date ({m.group(0)}) - state the constraint, not when it was measured"


@c_comment_rule("CALENDAR-DATE")
def rule_calendar_date_comment(root, path, text, comments):
    for c in comments:
        m = DATE.search(c.text)
        if m:
            yield c.line, f"comment carries a calendar date ({m.group(0)}) - state the constraint, not when it was measured"


# RULE: "living document" says only that the text changes, which git log
# already says, more precisely, for free.

LIVING_DOCUMENT = re.compile(r"living document", re.I)


@doc_rule("LIVING-DOC")
def rule_living_document(root, path, raw_lines):
    for number, line in enumerate(blank_fences(raw_lines), 1):
        if LIVING_DOCUMENT.search(line):
            yield number, '"living document" - say what the doc covers, not that it changes'


# RULE: includes are layer-qualified (docs/C-Style-Guide.md, "Headers and
# module boundaries"): "gfx/gfx.h", never "../../gfx/gfx.h" or a bare
# "gfx.h", even between two files in the same folder. launcher/main is a
# registered include root (INCLUDE_DIRS "." in CMakeLists.txt, and -I
# "$MAIN_DIR" in run_tests.sh), so every header under one of the shared
# layers, or living at main/'s own root, is reachable without a dot.
#
# resolve_include() finds the file the way the compiler does - next to the
# including file first, then from launcher/main - so an app's own local
# header (found next to it) is never mistaken for a layer header that
# merely shares its basename, and a fix always rewrites to the one spelling
# that file actually has.

INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"')


def resolve_include(root, path, inc):
    """Where the compiler actually finds a quoted include from `path`: next
    to the including file first (the real search order for
    `#include "..."`), then relative to launcher/main (its registered "."
    INCLUDE_DIRS root). None if neither has it - a real compile error, and
    not this rule's business."""
    main_dir = pathlib.Path(root) / "launcher/main"
    same_dir = (path.parent / inc).resolve()
    if same_dir.is_file():
        return same_dir
    from_root = (main_dir / inc).resolve()
    return from_root if from_root.is_file() else None


def _layer_root_files(root):
    return {p.name for p in (pathlib.Path(root) / "launcher/main").glob("*.h")}


def _include_layer_violations(root, path, text):
    """(line_no, include_string, canonical_spelling, message) for each
    include whose written spelling differs from the layer-qualified path
    the file it resolves to actually has."""
    main_dir = pathlib.Path(root) / "launcher/main"
    try:
        rel_to_main = path.resolve().relative_to(main_dir.resolve())
    except ValueError:
        return
    if rel_to_main.parts[:1] == ("apps",) and "tools" in rel_to_main.parts:
        return  # apps/*/tools/ is excluded from the firmware glob build entirely
    root_files = _layer_root_files(root)
    for number, line in enumerate(text.splitlines(), 1):
        m = INCLUDE.match(line)
        if not m:
            continue
        inc = m.group(1)
        resolved = resolve_include(root, path, inc)
        if resolved is None:
            continue
        try:
            target_rel = resolved.relative_to(main_dir.resolve())
        except ValueError:
            continue
        top = target_rel.parts[0] if target_rel.parts else ""
        canonical = target_rel.as_posix()
        if inc != canonical and (top in LAYER_DIRS or canonical in root_files):
            yield number, inc, canonical, f'"{inc}" is not layer-qualified - use "{canonical}"'


@c_line_rule("INCLUDE-LAYER", fixer=lambda root, path, text: _fix_include_layer(root, path, text))
def rule_include_layer(root, path, text):
    for number, _old, _new, message in _include_layer_violations(root, path, text):
        yield number, message


def _fix_include_layer(root, path, text):
    fixes = {old: new for _line, old, new, _msg in _include_layer_violations(root, path, text)}
    if not fixes:
        return None
    lines = text.splitlines(keepends=True)
    changed = False
    for i, line in enumerate(lines):
        m = INCLUDE.match(line)
        if m and m.group(1) in fixes:
            lines[i] = line.replace(f'"{m.group(1)}"', f'"{fixes[m.group(1)]}"', 1)
            changed = True
    return "".join(lines) if changed else None


# RULE: a folder may include only a strictly lower tier of
# docs/Launcher-Architecture.md's "How it fits together" (LAYER_TIER below;
# two folders can share a tier). app.h is outside LAYER_TIER, so its include of input/ is never checked;
# a system header such as "driver/temperature_sensor.h" never resolves to
# a layer.

LAYER_TIER = {"apps": 0, "boot": 1, "ui": 2, "console": 2, "gfx": 3, "render": 3,
             "display": 3, "input": 3, "util": 4, "board": 5}
LAYER_DIRS = tuple(layer for layer in LAYER_TIER if layer != "apps")

INCLUDE_DIRECTION_EXCEPTIONS = {}


def _layer_dirs_match(root):
    """A folder under launcher/main/ this table has never heard of makes
    every tier comparison below it a guess - fail loudly rather than
    silently trust an order that might already be wrong. A LAYER_TIER entry
    with no folder on disk (a fixture, or a layer deleted since) is not
    this check's business."""
    main_dir = pathlib.Path(root) / "launcher/main"
    found = {d.name for d in main_dir.iterdir() if d.is_dir()}
    unknown = sorted(found - set(LAYER_TIER))
    if unknown:
        raise ValueError(
            f"launcher/main has folder(s) {unknown} that LAYER_TIER (check_style_audit.py) "
            "does not know about - add them to the tier table (and to "
            "Launcher-Architecture.md's diagram) before this rule can trust its own order.")


@c_line_rule("INCLUDE-DIRECTION")
def rule_include_direction(root, path, text):
    main_dir = pathlib.Path(root) / "launcher/main"
    try:
        rel_to_main = path.resolve().relative_to(main_dir.resolve())
    except ValueError:
        return
    parts = rel_to_main.parts
    if not parts or parts[0] == "apps" or parts[0] not in LAYER_TIER:
        return
    _layer_dirs_match(root)
    source_layer = parts[0]
    source_tier = LAYER_TIER[source_layer]
    source_base = (rel_to_main.parent / rel_to_main.stem).as_posix()
    for number, line in enumerate(text.splitlines(), 1):
        m = INCLUDE.match(line)
        if not m:
            continue
        inc = m.group(1)
        resolved = resolve_include(root, path, inc)
        if resolved is None:
            continue
        try:
            target_rel = resolved.relative_to(main_dir.resolve())
        except ValueError:
            continue
        target_layer = target_rel.parts[0] if target_rel.parts else None
        if target_layer is None or target_layer == source_layer or target_layer not in LAYER_TIER:
            continue
        if LAYER_TIER[target_layer] > source_tier:
            continue
        if (source_base, target_layer) in INCLUDE_DIRECTION_EXCEPTIONS:
            continue
        yield number, (f'"{inc}" reaches from {source_layer}/ (tier {source_tier}) into {target_layer}/ '
                       f"(tier {LAYER_TIER[target_layer]}) - move the shared piece down a tier, or add an "
                       "INCLUDE_DIRECTION_EXCEPTIONS entry citing the doc section that draws it")


# RULE: a personal home-directory path, or one machine's ESP-IDF checkout,
# baked into tracked source only works on the machine that wrote it.
# launcher/tools/espressif.py's espressif_tools_root() and idf_python(), and
# idf.sh's idf_default_export(), are the portable forms.

# The ESP-IDF branch matches any drive path that names an esp-idf checkout,
# not one installer's folder layout. It stays off root-anchored paths: the
# portable $HOME/esp/esp-idf default and github.com/espressif/esp-idf both
# contain the name and are fine.
PERSONAL_PATH = re.compile(
    r"C:\\Users\\[A-Za-z0-9][A-Za-z0-9_.-]*|/home/[A-Za-z0-9][A-Za-z0-9_.-]*|/Users/[A-Za-z0-9][A-Za-z0-9_.-]*"
    r"|(?<![\w/])[A-Za-z]:[\\/][^\s\"'`;|()]*?esp-idf[\w.-]*")


@text_rule("PERSONAL-PATH")
def rule_personal_path(root, path, text):
    for number, line in enumerate(text.splitlines(), 1):
        m = PERSONAL_PATH.search(line)
        if m:
            yield number, f"{m.group(0)} is machine-specific - use ~, or launcher/tools/espressif.py or IDF_PATH, for an ESP-IDF path"


# RULE: an HTML comment in a doc renders as nothing. Only the gates' own
# escape markers and a generator's BEGIN/END GENERATED pair are allowed;
# anything else is a stray aside nobody will see. Scanned across the whole
# file (not line by line) so a comment wrapped across two lines is still
# caught, with fenced code blanked first so a shell transcript's own
# "<!--"-shaped text can't be mistaken for one.

HTML_COMMENT = re.compile(r"<!--.*?-->", re.S)
KNOWN_MARKERS = re.compile(
    re.escape(DOC_CONSTANTS_ESCAPE) + "|" + re.escape(DOC_VOCABULARY_ESCAPE) + r"|(?:BEGIN|END)\s+GENERATED")


# RULE: a working copy written with CRLF. .gitattributes normalises it on
# staging and CI reads the index, so this never reaches a commit - but a tool
# that rewrites a file whole leaves the checkout mixed, and the next reader
# sees a whole-file diff that is not there. A WARN with a fixer: --fix
# normalises it and nobody spends a thought on it.

LINE_ENDINGS_ACTION = "written with CRLF - run --fix, or git add --renormalize ."


@text_rule("LINE-ENDINGS", severity=WARN,
           fixer=lambda root, path, text: _fix_line_endings(path))
def rule_line_endings(root, path, text):
    raw = pathlib.Path(path).read_bytes()
    if b"\r\n" in raw:
        yield raw[:raw.index(b"\r\n")].count(b"\n") + 1, LINE_ENDINGS_ACTION


def _fix_line_endings(path):
    """Rewrites in place and returns None: the caller's text-in/text-out
    fixer contract would put the endings back on the way through."""
    path = pathlib.Path(path)
    raw = path.read_bytes()
    if b"\r\n" in raw:
        path.write_bytes(raw.replace(b"\r\n", b"\n"))
    return None


# RULE: `$?` on the line after a compound statement is that compound's own
# status, not the command a reader has in mind. An `if` whose condition fails
# and which has no `else` exits 0, so `if cmd; then return 0; fi` followed by
# `rc=$?` or `exit $?` reports success for a run that failed. Capture the
# status inside the branch instead: `cmd && return 0; rc=$?`.

SHELL_FILE = re.compile(r"\.sh$|\.bash$")
SHELL_SHEBANG = re.compile(r"^#!.*\b(?:ba|da|k|z)?sh\b")
COMPOUND_END = re.compile(r"^(?:fi|done|esac|\})\s*(?:#.*)?$")
STATUS_READ = re.compile(r"^(?:exit|return)\s+\$\?\s*$|^[A-Za-z_][A-Za-z0-9_]*=\$\?\s*$")

SHELL_STATUS_ACTION = ("$? here is the compound's status, not the command's - "
                       "capture it inside the branch (`cmd && return 0; rc=$?`)")


def _is_shell(path, text):
    return bool(SHELL_FILE.search(str(path))
                or SHELL_SHEBANG.match(text.splitlines()[0] if text else ""))


@text_rule("SHELL-COMPOUND-STATUS")
def rule_shell_compound_status(root, path, text):
    if not _is_shell(path, text):
        return
    previous = ""
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if STATUS_READ.match(line) and COMPOUND_END.match(previous):
            yield number, line + " after `" + previous + "`: " + SHELL_STATUS_ACTION
        previous = line


@doc_rule("STRAY-HTML-COMMENT")
def rule_stray_html_comment(root, path, raw_lines):
    blanked = "\n".join(blank_fences(raw_lines))
    for m in HTML_COMMENT.finditer(blanked):
        if KNOWN_MARKERS.search(m.group(0)):
            continue
        line = blanked.count("\n", 0, m.start()) + 1
        yield line, f"{m.group(0)[:60]} renders as nothing - delete it or say it in the text"


# RULE: a line starting "- " right after unindented, ordinary prose renders
# as a list item whether the author meant one or not. Walk back to the
# paragraph's own first line before deciding: CommonMark's lazy continuation
# lets a list item's text carry on with no indentation at all, so a
# paragraph that started with its own list marker is already a list, and a
# later "- " is a sibling item, not an accident. Skipped the same way for a
# paragraph that opens right after a closing code fence, a heading, a quote,
# or a table row, and when the line right above ends in ":" (which
# legitimately introduces a list).

BULLET = re.compile(r"^- \S")
LIST_MARKER = re.compile(r"^\s*([-*+]|\d+\.)\s")


@doc_rule("ACCIDENTAL-BULLET")
def rule_accidental_bullet(root, path, raw_lines):
    lines = blank_fences(raw_lines)
    for i, line in enumerate(lines):
        if i == 0 or not BULLET.match(line):
            continue
        prev = lines[i - 1]
        if prev.strip() == "":
            continue  # blank, or a blanked-out fenced line
        if prev != prev.lstrip():
            continue  # indented: continuing a nested block, not a bare paragraph
        if LIST_MARKER.match(prev):
            continue  # the previous line is itself a list item - a normal list
        start = i - 1
        while start > 0 and lines[start - 1].strip() != "":
            start -= 1
        para_first = lines[start]
        if LIST_MARKER.match(para_first) or para_first.lstrip().startswith(("#", ">", "|")):
            continue
        if prev.rstrip().endswith(":"):
            continue
        yield i + 1, 'starts "- " right after unindented prose - renders as a list item; rewrap the paragraph'


# RULE: two or more consecutive blank lines, or a trailing blank line at
# EOF. clang-format already owns this for C (MaxEmptyLinesToKeep, the LLVM
# default this repo never overrides, held tree-wide by format.yml on every
# push), so these only apply to docs. A generated table's own blank-line
# rhythm (docs/sand/Reaction-Table.md, between its BEGIN/END GENERATED
# markers) is the generator's to own, not this rule's.

def _generated_span(raw_lines):
    begin = end = None
    for i, line in enumerate(raw_lines):
        if "BEGIN GENERATED" in line:
            begin = i
        elif "END GENERATED" in line:
            end = i
    return (begin, end) if begin is not None and end is not None else None


@doc_rule("BLANK-LINES", fixer=lambda root, path, text: _fix_blank_lines(text))
def rule_blank_lines(root, path, raw_lines):
    span = _generated_span(raw_lines)
    run, fenced = 0, False
    for i, line in enumerate(raw_lines):
        if span and span[0] <= i <= span[1]:
            run = 0
            continue
        if line.lstrip().startswith("```"):
            fenced = not fenced
            run = 0
            continue
        if fenced:
            run = 0
            continue
        if line.strip() == "":
            run += 1
            if run == 2:
                yield i + 1, "two or more consecutive blank lines - use a single blank line"
        else:
            run = 0


def _fix_blank_lines(text):
    lines = text.splitlines(keepends=True)
    span = _generated_span([line.rstrip("\n") for line in lines])
    out, run, fenced = [], 0, False
    for i, line in enumerate(lines):
        if span and span[0] <= i <= span[1]:
            out.append(line)
            run = 0
            continue
        if line.lstrip().startswith("```"):
            fenced = not fenced
            run = 0
            out.append(line)
            continue
        if fenced:
            run = 0
            out.append(line)
            continue
        if line.strip() == "":
            run += 1
            if run > 1:
                continue
        else:
            run = 0
        out.append(line)
    new = "".join(out)
    return new if new != text else None


@doc_rule("TRAILING-BLANK-LINES", fixer=lambda root, path, text: _fix_trailing_blank(text))
def rule_trailing_blank_lines(root, path, raw_lines):
    trailing = 0
    for line in reversed(raw_lines):
        if line.strip() == "":
            trailing += 1
        else:
            break
    if trailing:
        yield len(raw_lines), f"{trailing} trailing blank line(s) at end of file - end the file on its last line of text"


def _fix_trailing_blank(text):
    lines = text.splitlines(keepends=True)
    trimmed = list(lines)
    while trimmed and trimmed[-1].strip() == "":
        trimmed.pop()
    if trimmed and not trimmed[-1].endswith("\n"):
        trimmed[-1] += "\n"
    new = "".join(trimmed)
    return new if new != text else None


# RULE: a drawn comment rule (`/*====`, `//----`) is decoration - OpenBSD
# style(9) has three comment shapes and none of them has one.
# scripts/gates/strip_comment_rules.py already proves the rewrite safe (code
# unchanged, prose unchanged word for word); this rule reuses its detection
# predicate (Comment.has_rule, from check_comment_length.py) for the check
# and its rewrite() for the fix, rather than a second implementation of
# either.

@c_comment_rule("DRAWN-COMMENT-RULE", fixer=lambda root, path, text: strip_comment_rules.rewrite(str(path), text))
def rule_drawn_comment(root, path, text, comments):
    for c in comments:
        if c.has_rule:
            yield c.line, "comment draws a rule (/*==== or //----) - style(9) has no such shape; delete the rule line (--fix does)"


# RULE: a one-line block comment of up to five plain words, sitting inside a
# function body, is a leftover section label for a block that should have
# been an extracted, named helper - docs/C-Style-Guide.md's "needing to
# comment parts of a function separately means the function is the
# problem." The identical shape is also this tree's normal way to divide a
# FILE's top-level declarations into named groups (gfx.c's own
# "/* Dirty tracking */" before its dirty-tracking statics), which is a
# different, accepted use - so this only fires once brace tracking proves
# the comment sits inside a real function body, not a top-level divider or
# initializer. WARN, not ERROR: whether a comment should exist at all is
# judgement (docs/C-Style-Guide.md's "Judgment rules"), and this is a
# worklist for a reader to weigh, not a verdict a script can hand down.

LABEL = re.compile(r"^[A-Z][a-z]+(?:\s[A-Za-z][a-z]*){0,4}$")


def _blank_comments_and_strings(text, comments):
    """`text` with every comment span (from `comments`) and every string/
    char literal blanked to spaces, newlines and length preserved - so a
    brace inside either can never affect the count below."""
    out = list(text)
    for c in comments:
        for start, end in c.spans:
            for i in range(start, end):
                if out[i] != "\n":
                    out[i] = " "
    i, n = 0, len(out)
    while i < n:
        ch = out[i]
        if ch in "\"'":
            quote, j = ch, i + 1
            while j < n and out[j] != quote:
                j += 2 if out[j] == "\\" and j + 1 < n else 1
            j = min(j + 1, n)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        i += 1
    return "".join(out)


def _function_body_comments(text, comments):
    """The subset of `comments` sitting inside a real function body: a '{'
    at brace-depth 0 opens one only when the character before it is ')' -
    the one shape a struct/array/enum initializer's '{' never has, since
    those follow '=' or a bare type keyword. Everything nested inside that
    frame - if/for/switch blocks, compound literals - inherits its state,
    counted in the one loop below."""
    blanked = _blank_comments_and_strings(text, comments)
    spans = {start: c for c in comments for start, _ in c.spans}
    inside = {}
    depth_is_fn, last_nonspace = [], ""
    for i, ch in enumerate(blanked):
        if i in spans:
            inside[id(spans[i])] = depth_is_fn[-1] if depth_is_fn else False
        if ch == "{":
            depth_is_fn.append(depth_is_fn[-1] if depth_is_fn else last_nonspace == ")")
        elif ch == "}":
            if depth_is_fn:
                depth_is_fn.pop()
        if not ch.isspace():
            last_nonspace = ch
    return {c for c in comments if inside.get(id(c))}


@c_comment_rule("HEADING-COMMENT", severity=WARN)
def rule_heading_comment(root, path, text, comments):
    in_function = _function_body_comments(text, comments)
    for c in comments:
        if c.is_banner or c.kind != "block" or c.lines != 1 or c not in in_function:
            continue
        t = c.text.strip()
        if LABEL.match(t):
            yield c.line, f'"{t}" is a section label inside a function body - extract a named helper instead'


def run_audit(root, rule_filter=None, file_filter=None):
    """Run every active rule's walker exactly once per matching file,
    returning (findings, files_scanned)."""
    active = [r for r in RULES if not rule_filter or r.id == rule_filter]
    findings = []
    scanned = set()

    def keep(path):
        return not file_filter or file_filter in str(path).replace("\\", "/")

    def add(rel, rule, item):
        line, message = item
        findings.append(Finding(rel, line, rule.id, rule.severity, message, rule.fixer is not None))

    doc_rules = [r for r in active if r.kind == "doc"]
    if doc_rules:
        for path, raw in _doc_walk(root):
            if not keep(path):
                continue
            rel = relpath(root, path)
            scanned.add(rel)
            for r in doc_rules:
                for item in (r.func(root, path, raw) or ()):
                    add(rel, r, item)

    c_rules = [r for r in active if r.kind in ("c_comment", "c_line")]
    if c_rules:
        for path, text in _c_walk(root):
            if not keep(path):
                continue
            rel = relpath(root, path)
            scanned.add(rel)
            comments = None
            for r in c_rules:
                if r.kind == "c_comment":
                    if comments is None:
                        comments = scan(rel, text)
                    items = r.func(root, path, text, comments)
                else:
                    items = r.func(root, path, text)
                for item in (items or ()):
                    add(rel, r, item)

    text_rules = [r for r in active if r.kind == "text"]
    if text_rules:
        for path, text in _text_walk(root):
            if not keep(path):
                continue
            rel = relpath(root, path)
            scanned.add(rel)
            for r in text_rules:
                for item in (r.func(root, path, text) or ()):
                    add(rel, r, item)

    findings.sort(key=lambda f: (f.path, f.line))
    return findings, len(scanned)


def run_fix(root, findings):
    """Apply every fixable finding's rule's fixer to the files it touched,
    file by file so multiple rules' fixes on one file compose."""
    by_id = {r.id: r for r in RULES}
    files = sorted({f.path for f in findings if f.fixable})
    fixed = 0
    for rel in files:
        path = pathlib.Path(root) / rel
        before = path.read_bytes()
        text = path.read_text(encoding="utf-8", errors="replace")
        original = text
        rule_ids = sorted({f.rule_id for f in findings if f.path == rel and f.fixable})
        for rule_id in rule_ids:
            fixer = by_id[rule_id].fixer
            new_text = fixer(root, path, text)
            if new_text is not None:
                text = new_text
        if text != original:
            path.write_text(text, encoding="utf-8", newline="")
        # A fixer may rewrite the file itself (line endings do not survive the
        # text contract above), so the count is what the bytes say.
        if path.read_bytes() != before:
            fixed += 1
            print(f"  fixed [{', '.join(rule_ids)}] {rel}")
    print(f"\nfixed {fixed} file(s)")


def main(argv):
    root = "."
    fix = strict = False
    rule_filter = file_filter = None
    it = iter(argv)
    for arg in it:
        if arg == "--fix":
            fix = True
        elif arg == "--strict":
            strict = True
        elif arg == "--rule":
            rule_filter = next(it, None)
        elif arg == "--file":
            file_filter = next(it, None)
        elif arg == "--root":
            root = next(it, None)
        else:
            print("usage: check_style_audit.py [--fix] [--strict] [--rule ID] [--file TEXT] [--root ROOT]",
                  file=sys.stderr)
            return 2

    if rule_filter and rule_filter not in {r.id for r in RULES}:
        print(f"no such rule: {rule_filter}", file=sys.stderr)
        return 2

    try:
        findings, scanned = run_audit(root, rule_filter, file_filter)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    if file_filter and scanned == 0:
        print(f"'{file_filter}' matched no file - nothing was audited.", file=sys.stderr)
        return 2

    if fix and any(f.fixable for f in findings):
        run_fix(root, findings)
        findings, scanned = run_audit(root, rule_filter, file_filter)

    for f in findings:
        print(f)
    errors = [f for f in findings if f.severity == ERROR]
    warnings = [f for f in findings if f.severity == WARN]
    print(f"{scanned} file(s) scanned, {len(errors)} error(s), {len(warnings)} warning(s)")
    return 1 if (errors or (strict and warnings)) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
