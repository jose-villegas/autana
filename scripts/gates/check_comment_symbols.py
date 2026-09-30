#!/usr/bin/env python3
"""Fail on a comment citing something that does not exist.

    python scripts/gates/check_comment_symbols.py [--require-idf] [root]

A trim that garbles a cited name leaves a comment pointing at nothing, which
is worse than the long comment it replaced. Four kinds of citation are
checked against the tree under `root`:

- `name()` must be declared or called in C code under `root` (vendored
  components included), or declared by ESP-IDF (for any chip it supports,
  the check's known limit) or its toolchain's C library
  (idf_vocabulary.outside_vocabulary()), or, when the comment names a `.py`
  file, defined by a Python script.
- A CONSTANT_NAME must be spelled somewhere other than a comment (see
  code_vocabulary.Vocabulary). Only project families are checked: a name
  whose first word no project #define or Kconfig option starts with
  (ESP_FAIL, CONFIG_PM_ENABLE) is the SDK's.
- A name pinned to a file, `name() (file.c)`, `NAME, file.h)`,
  `name()'s own comment, file.c)`, must be spelled in that file's code.
- A quoted all-caps heading in citing form (`file.h's "HEADING"`,
  `("HEADING")`) must appear in some other comment.

Only the last is text matching; the rest ask the code. None of them can
tell whether what a comment SAYS about a real name is still true.

Without an ESP-IDF checkout a `name()` the tree does not define is counted,
not failed, and one line says so; --require-idf, which CI passes, makes a
missing checkout, or a missing toolchain C library, an error instead.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from code_vocabulary import CONSTANT, family, vocabulary  # noqa: E402
from check_comment_length import EXCLUDED, scan  # noqa: E402
from idf_vocabulary import not_verified_notice, outside_vocabulary, required_missing  # noqa: E402
from tracked import committable  # noqa: E402

CITED = re.compile(r"\b([a-z_][a-z0-9_]*)\(\)")
PINNED = re.compile(r"\b([a-z_][a-z0-9_]*\(\)|[A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]*)"
                    r"(?:'s own [^(),]{0,40})?\s*(?:\(|,\s*)(?:see\s+)?`?([\w./-]+\.[ch])`?\)")
HEADING = re.compile(r"(?:'s(?: own)?\s+|\(\s*)\"([A-Z][A-Z0-9 ,'/-]*[A-Z0-9])\"")

def comments(root):
    """Every checked comment under `root`, as (path, Comment), in files git
    would commit: a local venv's headers cite names CI never sees."""
    for p in committable(root):
        if p.suffix not in (".c", ".h"):
            continue
        rp = p.as_posix()
        if any(rp.startswith(e) for e in EXCLUDED):
            continue
        for c in scan(rp, p.read_text(encoding="utf-8", errors="replace")):
            yield rp, c


def problems(root, outside, unchecked=None):
    """Every unresolved citation under `root`, as "path:line: message".
    `outside` is an idf_vocabulary.OutsideVocabulary, or None when there is
    no ESP-IDF to ask: a `name()` the tree lacks then goes into `unchecked`
    instead of the result."""
    vocab = vocabulary(root)
    found = list(comments(root))
    headings = " \n".join(c.text for _, c in found)
    out = []
    unchecked = [] if unchecked is None else unchecked
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
            if name in vocab.functions or (outside is not None and name in outside.functions):
                continue
            if name in vocab.script_functions and ".py" in text:
                continue
            if outside is None:
                unchecked.append(f"{where}: {name}()")
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


def main(argv):
    require_idf = "--require-idf" in argv
    argv = [arg for arg in argv if arg != "--require-idf"]
    root = argv[0] if argv else "launcher"
    outside = outside_vocabulary()
    if require_idf and required_missing(outside):
        print(required_missing(outside), file=sys.stderr)
        return 2
    unchecked = []
    found = problems(root, outside, unchecked)
    for line in found:
        print(line)
    if outside is None:
        print(not_verified_notice(len(unchecked)))
    print(f"{len(found)} unresolved citations")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
