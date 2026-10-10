#!/usr/bin/env python3
"""Fail on a comment or engine document below `apps/` that names a particular app.

    python scripts/gates/check_comment_layers.py [--context] [--paths FILE...]

An app is a folder designed to be deleted whole, so a comment in a lower layer
naming one is a dangling reference by construction: delete the app and the
comment survives the code it described. Say what shape of caller needs the
thing, or state the rule a caller must follow. An app's own files may name
anything below them; that direction cannot dangle.

The engine's documents sit at the same layer as its code, so every line of
them is held to the same rule: every Markdown file under docs/ except an app's
own folder (docs/<app>/) and docs/plans/, whose designs span layers and name
the apps they plan for, plus any Markdown file inside a lower layer.

App names come from the folders themselves, so adding an app extends the check.

A name is all this checks, including a name spelled as part of a compound
identifier or filename (`app_sand.c`, `sand_ui_step()`, `sand_palette256.h`),
not just the bare word, and with an underscore in the folder name spelled any
way prose spells it: `render_lab`, "Render Lab", `render-lab.png`.

Borrowing an app's VOCABULARY is the same fault one step quieter; "an app's
working grid" names no app but still assumes apps have grids, and gfx has no
concept of a grain, but no word list can judge it: the dirty tracker really
does have a grid of cells, and a font really does have a glyph cell. That one
is read, not scripted.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from c_comments import sources as comment_sources, scan  # noqa: E402
from tracked import tracked_files  # noqa: E402
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "launcher/tools"))
from build.packages import FIRST_PARTY  # noqa: E402

# Below the apps: all of launcher/main/ except apps/, the packages and the
# test tree. A new folder is checked from the day it exists.
ENGINE = (*FIRST_PARTY, "launcher/test/")
APPS = "launcher/main/apps"


def below_apps(rp):
    return rp.startswith(ENGINE) and not rp.startswith(APPS + "/")

# The diagnostics build is the variant behind build.diag, which every layer
# may name; the app that happens to share the word is what this forbids.
# Whitespace includes a newline, since prose wraps anywhere.
VARIANT = re.compile(
    r"\b(?:diagnostics[\s-]+(?:builds?|variants?|images?)|build-diagnostics)\b", re.I)


def app_names(root="."):
    return sorted(p.name for p in (pathlib.Path(root) / APPS).iterdir() if p.is_dir())


def name_pattern(names):
    # Boundaries exclude only [A-Za-z0-9], not underscore, so a name inside a
    # compound identifier like `app_sand.c` or `sand_ui_step` still matches.
    spelled = [re.escape(n).replace("_", r"(?:\s+|[_-]|%5[fF]|%2[dD])?") for n in names]
    return re.compile(r"(?<![A-Za-z0-9])(" + "|".join(spelled) + r")(?![A-Za-z0-9])", re.I)


def canonical(hit, names):
    flat = re.sub(r"[\s_-]+|%5f|%2d", "", hit.lower())
    return next(n for n in names if n.replace("_", "") == flat)


def sources(root):
    for path in comment_sources(root, tracked=True):
        rp = path.relative_to(root).as_posix()
        if below_apps(rp):
            yield rp

def documents(root, names):
    own = ("docs/plans/",) + tuple(f"docs/{n}/" for n in names)
    for rp in tracked_files(root):
        if rp.endswith(".md") and (rp.startswith("docs/") or below_apps(rp)) and not rp.startswith(own):
            yield rp


def problems(root=".", context=False, files=None):
    names = app_names(root)
    word = name_pattern(names)

    def hits(text):
        return word.finditer(VARIANT.sub(lambda m: " " * len(m.group()), text))

    def read(rp):
        return (pathlib.Path(root) / rp).read_text(encoding="utf-8", errors="replace")

    found = []
    for rp in sources(root):
        if files is not None and rp not in files:
            continue
        for c in scan(rp, read(rp)):
            named = sorted(set(canonical(m.group(), names) for m in hits(c.text)))
            if named:
                found.append(f"{rp}:{c.line}: comment names {', '.join(named)}")
                if context:
                    found.append(f"      {c.text[:160]}")
    for rp in documents(root, names):
        if files is not None and rp not in files:
            continue
        text = read(rp)
        lines = text.splitlines()
        for m in hits(text):
            line = text.count("\n", 0, m.start()) + 1
            found.append(f"{rp}:{line}: document names {canonical(m.group(), names)}")
            if context:
                found.append(f"      {lines[line - 1].strip()[:160]}")
    return found


def main():
    args = sys.argv[1:]
    files = args[args.index("--paths") + 1:] if "--paths" in args else None
    found = problems(context="--context" in args, files=files)
    for line in found:
        print(line)
    count = sum(1 for line in found if not line.startswith(" "))
    print(f"{count} line{'' if count == 1 else 's'} below apps/ "
          f"name an app ({', '.join(app_names())})")
    return 1 if count else 0


if __name__ == "__main__":
    sys.exit(main())
