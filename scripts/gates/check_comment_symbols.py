#!/usr/bin/env python3
"""Fail on a comment citing something that does not exist.

    python scripts/gates/check_comment_symbols.py [root]

A trim that garbles a cited name leaves a comment pointing at nothing, which
is worse than the long comment it replaced. Four kinds of citation are
checked against the tree under `root`:

- `name()` must be declared or called in C code, or, when the comment names
  a `.py` file, defined by a Python script.
- A CONSTANT_NAME must be spelled somewhere other than a comment (see
  code_vocabulary.Vocabulary). Only project families are checked: a name
  whose first word no project #define or Kconfig option starts with
  (ESP_FAIL, CONFIG_PM_ENABLE) is the SDK's.
- A name pinned to a file - `name() (file.c)`, `NAME, file.h)`,
  `name()'s own comment, file.c)` - must be spelled in that file's code.
- A quoted all-caps heading in citing form - `file.h's "HEADING"`,
  `("HEADING")` - must appear in some other comment.

Only the last is text matching; the rest ask the code. None of them can
tell whether what a comment SAYS about a real name is still true.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from code_vocabulary import CONSTANT, family, vocabulary  # noqa: E402
from check_comment_length import EXCLUDED, scan  # noqa: E402

SKIP = ("build", "build.dev", "build.diag", "build.qemu", "build.qemu.perf", "build.qemu.shell", "managed_components")
CITED = re.compile(r"\b([a-z_][a-z0-9_]*)\(\)")
PINNED = re.compile(r"\b([a-z_][a-z0-9_]*\(\)|[A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]*)"
                    r"(?:'s own [^(),]{0,40})?\s*(?:\(|,\s*)(?:see\s+)?`?([\w./-]+\.[ch])`?\)")
HEADING = re.compile(r"(?:'s(?: own)?\s+|\(\s*)\"([A-Z][A-Z0-9 ,'/-]*[A-Z0-9])\"")

# Named in comments as the C library or the vendor SDK spells them, with no
# definition in this tree to find.
FOREIGN = {"printf", "malloc", "bsp_display_new", "esp_clk_cpu_freq", "xtensa_perfmon_exec",
           "strcasecmp", "strdup", "free", "main", "exit", "sin", "cos", "fabs", "rand", "hypot",
           "open"}


def scan_citations(root):
    """Every `name()` cited in a comment under `root`, FOREIGN included, as
    {name: {"path:line", ...}}."""
    cited = {}
    for p in sorted(pathlib.Path(root).rglob("*")):
        if p.suffix not in (".c", ".h") or any(s in p.parts for s in SKIP):
            continue
        rp = p.as_posix()
        if any(rp.startswith(e) for e in EXCLUDED):
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        for c in scan(rp, text):
            for name in CITED.findall(c.text):
                cited.setdefault(name, set()).add(f"{rp}:{c.line}")
    return cited


def stale_foreign(root):
    """FOREIGN entries no comment under `root` actually cites - dead
    allowlisting, the same trap an unused doc-citation allowlist entry is."""
    cited = scan_citations(root)
    return sorted(name for name in FOREIGN if name not in cited)


def comments(root):
    """Every checked comment under `root`, as (path, Comment)."""
    for p in sorted(pathlib.Path(root).rglob("*")):
        if p.suffix not in (".c", ".h") or any(s in p.parts for s in SKIP):
            continue
        rp = p.as_posix()
        if any(rp.startswith(e) for e in EXCLUDED):
            continue
        for c in scan(rp, p.read_text(encoding="utf-8", errors="replace")):
            yield rp, c


def problems(root):
    """Every unresolved citation under `root`, as "path:line: message"."""
    vocab = vocabulary(root)
    found = list(comments(root))
    headings = " \n".join(c.text for _, c in found)
    out = []
    for rp, c in found:
        text = c.text
        where = f"{rp}:{c.line}"
        for m in CITED.finditer(text):
            name = m.group(1)
            before = text[:m.start()]
            if name.startswith("_") and before.rstrip().endswith("/"):
                continue  # begin()/_end(): a suffix of the name before it
            if before.endswith("."):
                continue  # json.loads(): a method, not a name this tree owns
            if name in vocab.functions or name in FOREIGN:
                continue
            if name in vocab.script_functions and ".py" in text:
                continue
            reason = ("is defined only by a script the comment does not name"
                      if name in vocab.script_functions else "does not exist")
            out.append(f"{where}: comment names {name}(), which {reason}")
        for name in CONSTANT.findall(text):
            if family(name) in vocab.families and name not in vocab.constants:
                out.append(f"{where}: comment names {name}, which does not exist")
        for m in PINNED.finditer(text):
            name, file = m.group(1).removesuffix("()"), pathlib.PurePath(m.group(2)).name
            spelled = vocab.defined_in.get(file)
            if spelled is None:
                out.append(f"{where}: comment places {name} in {m.group(2)}, which does not exist")
            elif name not in spelled:
                out.append(f"{where}: comment places {name} in {file}, whose code never names it")
        for heading in HEADING.findall(text):
            if " " in heading and headings.count(heading) < 2:
                out.append(f'{where}: comment cites "{heading}", which no other comment has')
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "launcher"
    found = problems(root)
    stale = stale_foreign(root)
    for line in found:
        print(line)
    for name in stale:
        print(f"FOREIGN: stale entry {name}: no comment cites it")
    print(f"{len(found)} unresolved citations, "
          f"{len(stale)} stale FOREIGN entr{'y' if len(stale) == 1 else 'ies'}")
    return 1 if found or stale else 0


if __name__ == "__main__":
    sys.exit(main())
