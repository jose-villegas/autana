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
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_comment_length import EXCLUDED, scan  # noqa: E402
from check_doc_citations import SKIP as SKIP_DIRS, documentation  # noqa: E402
from check_doc_constants import ESCAPE as DOC_CONSTANTS_ESCAPE  # noqa: E402
from check_doc_vocabulary import ESCAPE as DOC_VOCABULARY_ESCAPE  # noqa: E402
import strip_comment_rules  # noqa: E402

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


# Every rule function takes (root, path, data) and yields (line, message)
# pairs; `data` is the one thing its walker already read for it - raw doc
# lines, a file's scanned comments, or its raw text. Every fixer takes
# (root, path, text) and returns the file's new text, or None to leave it
# alone - the same shape regardless of which walker found the violation, so
# --fix does not need to know which kind of rule it is re-running.
doc_rule = _register("doc")
c_comment_rule = _register("c_comment")
c_line_rule = _register("c_line")
text_rule = _register("text")


def tracked_files(root, patterns):
    result = subprocess.run(["git", "ls-files", *patterns], cwd=root,
                            capture_output=True, text=True, check=True)
    return [line for line in result.stdout.splitlines() if line]


def relpath(root, path):
    return pathlib.Path(path).resolve().relative_to(pathlib.Path(root).resolve()).as_posix()


def strip_fences(lines):
    fenced = False
    for number, line in enumerate(lines, 1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        yield number, line


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
        if any(part in SKIP_DIRS for part in pathlib.PurePosixPath(rel).parts):
            continue
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
        if p.suffix.lower() in BINARY_EXTENSIONS or any(part in SKIP_DIRS for part in p.parts):
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
# in tracked text. A bd id is always written "bd autana-<code>" in this
# tree; a bare "autana-cli"-shaped compound word (this doc's own CLI,
# "autana-screenshot", "autana-device", ...) is not one, so only a
# digit-led bare code is also flagged - no real compound word in this tree
# starts with a digit. A 40-character hex run is an unambiguous full SHA; a
# shorter one is not (see suite_sand_perf.c's own build-id comments, which
# are perf provenance, not a tracker reference).

TRACKER_ID = re.compile(
    r"\bbd\s+(?:show\s+|graph\s+)?autana-[a-zA-Z0-9]+\b|\bautana-[0-9][a-z0-9]{1,5}\b")
COMMIT_SHA = re.compile(r"\b[0-9a-f]{40}\b")


def _tracker_refs(text):
    for pattern, what in ((TRACKER_ID, "a tracker issue id"), (COMMIT_SHA, "a git commit hash")):
        m = pattern.search(text)
        if m:
            yield what, m.group(0)


@doc_rule("TRACKER-REF")
def rule_tracker_ref_doc(root, path, raw_lines):
    for number, line in strip_fences(raw_lines):
        for what, value in _tracker_refs(line):
            yield number, f"names {what} ({value}) - issue ids and commit hashes never go into tracked text"


@c_comment_rule("TRACKER-REF")
def rule_tracker_ref_comment(root, path, comments):
    for c in comments:
        for what, value in _tracker_refs(c.text):
            yield c.line, f"comment names {what} ({value}) - issue ids and commit hashes never go into tracked text"


# RULE: a calendar date in a C comment or a doc records when something was
# true, not what is true now. docs/doc_review_ledger.txt is a genuine dated
# log (path, date, note - one row per review); it is a .txt, outside this
# rule's .md/.c/.h scope, which is exactly the exception that needs one.

DATE = re.compile(r"\b(?:19|20)\d{2}-\d{2}-\d{2}\b")


@doc_rule("CALENDAR-DATE")
def rule_calendar_date_doc(root, path, raw_lines):
    for number, line in strip_fences(raw_lines):
        m = DATE.search(line)
        if m:
            yield number, f"carries a calendar date ({m.group(0)}) - state the constraint, not when it was measured"


@c_comment_rule("CALENDAR-DATE")
def rule_calendar_date_comment(root, path, comments):
    for c in comments:
        m = DATE.search(c.text)
        if m:
            yield c.line, f"comment carries a calendar date ({m.group(0)}) - state the constraint, not when it was measured"


# RULE: "living document" says only that the text changes, which git log
# already says, more precisely, for free.

LIVING_DOCUMENT = re.compile(r"living document", re.I)


@doc_rule("LIVING-DOC")
def rule_living_document(root, path, raw_lines):
    for number, line in strip_fences(raw_lines):
        if LIVING_DOCUMENT.search(line):
            yield number, '"living document" - say what the doc covers, not that it changes'


# RULE: includes are layer-qualified (docs/C-Style-Guide.md, "Headers and
# module boundaries"): "gfx/gfx.h", never "../../gfx/gfx.h" or a bare
# "gfx.h", even between two files in the same folder. launcher/main is a
# registered include root (INCLUDE_DIRS "." in CMakeLists.txt, and -I
# "$MAIN_DIR" in run_tests.sh), so every header under one of the shared
# layers, or living at main/'s own root, is reachable without a dot. Scoped
# to those shared layers only - docs/Launcher-Architecture.md's own
# "How it fits together" is about an app reaching down past ui/ into gfx/,
# not about an app's own internal includes, and apps organise their own
# folders however they like.
#
# The detection predicate is shared by the check and the fix: a bare form is
# only auto-fixed when its basename names exactly one layer header - two
# files sharing a basename in different layers would make the fix a guess,
# so that case is reported, never rewritten.

INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"')
LAYER_DIRS = ("boot", "display", "gfx", "input", "render", "ui", "util", "console")
LAYER_ROOT_FILES = ("app.h", "build_variant.h")

_layer_header_cache = {}


def _layer_headers(root):
    key = str(root)
    if key not in _layer_header_cache:
        main_dir = pathlib.Path(root) / "launcher/main"
        headers = {}
        for layer in LAYER_DIRS:
            for p in (main_dir / layer).rglob("*.h"):
                headers.setdefault(p.name, []).append(f"{layer}/{p.relative_to(main_dir / layer).as_posix()}")
        _layer_header_cache[key] = headers
    return _layer_header_cache[key]


def _include_layer_violations(root, path, text):
    """(line_no, include_string, replacement_or_None, message) for each
    relative or unqualified include of a shared-layer header."""
    main_dir = pathlib.Path(root) / "launcher/main"
    try:
        rel_to_main = path.resolve().relative_to(main_dir.resolve())
    except ValueError:
        return
    if rel_to_main.parts[:1] == ("apps",) and "tools" in rel_to_main.parts:
        return  # apps/*/tools/ is excluded from the firmware glob build entirely
    layer_headers = _layer_headers(root)
    for number, line in enumerate(text.splitlines(), 1):
        m = INCLUDE.match(line)
        if not m:
            continue
        inc = m.group(1)
        if "/" not in inc and inc in layer_headers:
            candidates = layer_headers[inc]
            if len(candidates) == 1:
                yield number, inc, candidates[0], f'"{inc}" names a layer header without its folder - use "{candidates[0]}"'
            else:
                yield number, inc, None, f'"{inc}" names a layer header without its folder, and is ambiguous ({", ".join(candidates)})'
            continue
        if "../" in inc:
            resolved = (path.parent / inc).resolve()
            try:
                target_rel = resolved.relative_to(main_dir.resolve())
            except ValueError:
                continue
            top = target_rel.as_posix().split("/")[0]
            if resolved.exists() and (top in LAYER_DIRS or target_rel.as_posix() in LAYER_ROOT_FILES):
                fix = target_rel.as_posix()
                yield number, inc, fix, f'"{inc}" is relative - use "{fix}"'


@c_line_rule("INCLUDE-LAYER", fixer=lambda root, path, text: _fix_include_layer(root, path, text))
def rule_include_layer(root, path, text):
    for number, _old, new, message in _include_layer_violations(root, path, text):
        yield number, message, new is not None


def _fix_include_layer(root, path, text):
    fixes = {old: new for _line, old, new, _msg in _include_layer_violations(root, path, text) if new}
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


# RULE: a folder may include anything strictly below it in
# docs/Launcher-Architecture.md's "How it fits together" tier diagram, and
# app.h, never above or sideways into a different same-tier folder. Order
# is the one table below (folder membership alone does not fix it - two
# folders can share a tier); INCLUDE_DIRECTION_EXCEPTIONS is every reach the
# diagram itself documents as deliberate, each citing the section that says
# so. app.h's own reach into input/buttons.h needs no entry: app.h sits
# above every tier, so "anything below it" already covers it - the diagram
# calls it out only because a slim shell contract depending on a driver
# header is worth a reader noticing, not because it is illegal.
#
# util/device_state.c also opens ESP-IDF's own "driver/temperature_sensor.h"
# - a system header, never a first-party board/ one, so it never reaches
# this rule's resolution step at all; "a driver header, not drawn" in the
# prose is this file's own name for exactly that gap.
#
# PR #345 (claude/layering-hygiene) moves input_t into a new input/input.h
# and device_state's temperature read into board/ - this table is correct
# for main as it stands now, and #345 will need to retarget the
# util/device_state.h -> input/imu.h entry once input_t moves.

LAYER_TIER = {"boot": 1, "ui": 2, "console": 2, "gfx": 3, "render": 3, "display": 3,
             "input": 3, "board": 4, "util": 4}

_ARCH_SECTION = 'Launcher-Architecture.md, "How it fits together"'
INCLUDE_DIRECTION_EXCEPTIONS = {
    ("util/device_state.c", "display/display.h"): _ARCH_SECTION,
    ("util/device_state.h", "input/imu.h"): _ARCH_SECTION,
}


def _layer_dirs_match(root):
    """A folder under launcher/main/ this table has never heard of makes
    every tier comparison below it a guess - fail loudly rather than
    silently trust an order that might already be wrong. A LAYER_TIER entry
    with no folder on disk (a fixture, or a layer deleted since) is not
    this check's business."""
    main_dir = pathlib.Path(root) / "launcher/main"
    found = {d.name for d in main_dir.iterdir() if d.is_dir() and d.name != "apps"}
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
    if not parts or parts[0] not in LAYER_TIER:
        return
    _layer_dirs_match(root)
    source_layer = parts[0]
    source_tier = LAYER_TIER[source_layer]
    source_key = rel_to_main.as_posix()
    for number, line in enumerate(text.splitlines(), 1):
        m = INCLUDE.match(line)
        if not m:
            continue
        inc = m.group(1)
        if "/" not in inc:
            continue
        target_layer = inc.split("/", 1)[0]
        if target_layer == source_layer or target_layer not in LAYER_TIER:
            continue
        if LAYER_TIER[target_layer] > source_tier:
            continue
        if INCLUDE_DIRECTION_EXCEPTIONS.get((source_key, inc)):
            continue
        yield number, (f'"{inc}" reaches from {source_layer}/ (tier {source_tier}) into {target_layer}/ '
                       f"(tier {LAYER_TIER[target_layer]}) - a folder may only include a strictly lower tier")


# RULE: a personal home-directory path baked into tracked source only works
# on the machine that wrote it. `~` (or launcher/tools/idf_python.py's own
# version-glob lookup) is the portable form this tree already uses.

PERSONAL_PATH = re.compile(
    r"C:\\Users\\[A-Za-z0-9][A-Za-z0-9_.-]*|/home/[A-Za-z0-9][A-Za-z0-9_.-]*|/Users/[A-Za-z0-9][A-Za-z0-9_.-]*")


@text_rule("PERSONAL-PATH")
def rule_personal_path(root, path, text):
    for number, line in enumerate(text.splitlines(), 1):
        m = PERSONAL_PATH.search(line)
        if m:
            yield number, f"{m.group(0)} is machine-specific - use ~ and a version glob"


# RULE: an HTML comment in a doc renders as nothing. Only the gates' own
# escape markers and a generator's BEGIN/END GENERATED pair are allowed;
# anything else is a stray aside nobody will see. Scanned across the whole
# file (not line by line) so a comment wrapped across two lines is still
# caught, with fenced code blanked first so a shell transcript's own
# "<!--"-shaped text can't be mistaken for one.

HTML_COMMENT = re.compile(r"<!--.*?-->", re.S)
KNOWN_MARKERS = re.compile(
    re.escape(DOC_CONSTANTS_ESCAPE) + "|" + re.escape(DOC_VOCABULARY_ESCAPE) + r"|(?:BEGIN|END)\s+GENERATED")


def _blank_fences(raw_lines):
    fenced = False
    out = []
    for line in raw_lines:
        if line.lstrip().startswith("```"):
            fenced = not fenced
            out.append("")
            continue
        out.append("" if fenced else line)
    return out


@doc_rule("STRAY-HTML-COMMENT")
def rule_stray_html_comment(root, path, raw_lines):
    blanked = "\n".join(_blank_fences(raw_lines))
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
    fenced = False
    for i, line in enumerate(raw_lines):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced or i == 0 or not BULLET.match(line):
            continue
        prev = raw_lines[i - 1]
        if prev.strip() == "" or prev.lstrip().startswith("```"):
            continue
        if prev != prev.lstrip():
            continue  # indented: continuing a nested block, not a bare paragraph
        if LIST_MARKER.match(prev):
            continue  # the previous line is itself a list item - a normal list
        start = i - 1
        while (start > 0 and raw_lines[start - 1].strip() != ""
               and not raw_lines[start - 1].lstrip().startswith("```")):
            start -= 1
        para_first = raw_lines[start]
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
    run = 0
    for i, line in enumerate(raw_lines):
        if span and span[0] <= i <= span[1]:
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
    out, run = [], 0
    for i, line in enumerate(lines):
        if span and span[0] <= i <= span[1]:
            out.append(line)
            run = 0
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
        yield len(raw_lines), f"{trailing} trailing blank line(s) at end of file"


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
def rule_drawn_comment(root, path, comments):
    for c in comments:
        if c.has_rule:
            yield c.line, "comment draws a rule (/*==== or //----) - style(9) has no such shape"


# RULE: a one-line Title Case comment with no sentence, sitting inside a
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
def rule_heading_comment(root, path, comments):
    text = path.read_text(encoding="utf-8", errors="replace")
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
        # A rule's check yields (line, message) or (line, message, fixable) -
        # the third field overrides "this rule has a fixer" for a specific
        # finding, the way INCLUDE-LAYER's ambiguous case must: the rule can
        # fix ONE shape of its own violation, not every one it reports.
        line, message, *rest = item
        fixable = (rule.fixer is not None) and (rest[0] if rest else True)
        findings.append(Finding(rel, line, rule.id, rule.severity, message, fixable))

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
                    data = comments
                else:
                    data = text
                for item in (r.func(root, path, data) or ()):
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

    findings, scanned = run_audit(root, rule_filter, file_filter)
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
