#!/usr/bin/env python3
"""Fail on a comment or engine document below `apps/` that names a particular app.

    python scripts/gates/check_comment_layers.py [--context]

An app is a folder designed to be deleted whole, so a comment in a lower layer
naming one is a dangling reference by construction: delete the app and the
comment survives the code it described. Say what shape of caller needs the
thing, or state the rule a caller must follow. An app's own files may name
anything below them - that direction cannot dangle.

The engine's documents sit at the same layer as its code, so every line of
them is held to the same rule: every Markdown file under docs/ except an app's
own folder (docs/<app>/) and docs/plans/, whose designs span layers and name
the apps they plan for, plus any Markdown file inside a lower layer.

App names come from the folders themselves, so adding an app extends the check.

A name is all this checks, including a name spelled as part of a compound
identifier or filename - `app_sand.c`, `sand_ui_step()`, `sand_palette256.h` -
not just the bare word, and with an underscore in the folder name spelled any
way prose spells it: `render_lab`, "Render Lab", `render-lab.png`.

Borrowing an app's VOCABULARY is the same fault one step quieter - "an app's
working grid" names no app but still assumes apps have grids, and gfx has no
concept of a grain - but no word list can judge it: the dirty tracker really
does have a grid of cells, and a font really does have a glyph cell. That one
is read, not scripted.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_comment_length import EXCLUDED, scan  # noqa: E402

LOWER = ("launcher/main/boot/", "launcher/main/display/", "launcher/main/gfx/",
         "launcher/main/input/", "launcher/main/render/", "launcher/main/ui/",
         "launcher/main/util/", "launcher/main/console/", "launcher/test/")
SKIP = ("build", "build.dev", "build.diag", "build.qemu", "build.qemu.perf", "build.qemu.shell", "managed_components")
APPS = "launcher/main/apps"

# The shell's own two files: they switch between apps without knowing one.
SHELL = ("launcher/main/app.h", "launcher/main/main.c")

# The diagnostics build is the variant behind build.diag, which every layer
# may name; the app that happens to share the word is what this forbids.
VARIANT = re.compile(
    r"\b(?:diagnostics[ -](?:builds?|variants?|images?)|build-diagnostics)\b", re.I)


def app_names(root="."):
    return sorted(p.name for p in (pathlib.Path(root) / APPS).iterdir() if p.is_dir())


def name_pattern(names):
    # Boundaries exclude only [A-Za-z0-9], not underscore, so a name inside a
    # compound identifier like `app_sand.c` or `sand_ui_step` still matches.
    spelled = [re.escape(n).replace("_", "[ _-]?") for n in names]
    return re.compile(r"(?<![A-Za-z0-9])(" + "|".join(spelled) + r")(?![A-Za-z0-9])", re.I)


def canonical(hit, names):
    flat = re.sub(r"[ _-]", "", hit.lower())
    return next(n for n in names if n.replace("_", "") == flat)


def tracked(root):
    for p in sorted(pathlib.Path(root).rglob("*")):
        rel = p.relative_to(root)
        if p.is_file() and not any(s in rel.parts for s in SKIP):
            yield rel.as_posix(), p


def sources(root):
    for rp, p in tracked(pathlib.Path(root) / "launcher"):
        rp = "launcher/" + rp
        if p.suffix not in (".c", ".h") or any(rp.startswith(e) for e in EXCLUDED):
            continue
        if rp.startswith(LOWER) or rp in SHELL:
            yield rp, p


def documents(root, names):
    own = ("docs/plans/",) + tuple(f"docs/{n}/" for n in names)
    for top in ("docs", "launcher"):
        for rp, p in tracked(pathlib.Path(root) / top):
            rp = f"{top}/{rp}"
            if p.suffix != ".md" or rp.startswith(own):
                continue
            if top == "docs" or rp.startswith(LOWER):
                yield rp, p


def problems(root=".", context=False):
    names = app_names(root)
    word = name_pattern(names)

    def named(text):
        return sorted(set(canonical(m, names) for m in word.findall(VARIANT.sub("", text))))

    found = []
    for rp, p in sources(root):
        for c in scan(rp, p.read_text(encoding="utf-8", errors="replace")):
            hits = named(c.text)
            if hits:
                found.append(f"{rp}:{c.line}: comment names {', '.join(hits)}")
                if context:
                    found.append(f"      {c.text[:160]}")
    for rp, p in documents(root, names):
        lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        for n, line in enumerate(lines, 1):
            hits = named(line)
            if hits:
                found.append(f"{rp}:{n}: document names {', '.join(hits)}")
                if context:
                    found.append(f"      {line.strip()[:160]}")
    return found


def main():
    found = problems(context="--context" in sys.argv[1:])
    for line in found:
        print(line)
    count = sum(1 for line in found if not line.startswith(" "))
    print(f"{count} line{'' if count == 1 else 's'} below apps/ "
          f"name an app ({', '.join(app_names())})")
    return 1 if count else 0


if __name__ == "__main__":
    sys.exit(main())
