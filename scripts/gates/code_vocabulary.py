"""Names declared by the engine and its checked-in maintenance scripts."""
import pathlib
import re

SKIP = {"build", "build.dev", "build.diag", "build.qemu", "build.qemu.perf", "build.qemu.shell", "managed_components", ".git"}
SOURCE_SUFFIXES = {".c", ".h", ".py"}
C_SUFFIXES = {".c", ".h"}
FUNCTION = re.compile(r"\b([a-z_][a-z0-9_]{4,})\s*\(")
MACRO = re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]+)\b", re.M)
KCONFIG = re.compile(r"^\s*config\s+([A-Z][A-Z0-9_]+)\b", re.M)
UPPERCASE = re.compile(r"\b([A-Z][A-Z0-9_]+)\b")


def source_paths(root):
    root = pathlib.Path(root)
    bases = (root / "launcher", root / "scripts")
    if root.name == "launcher":
        bases = (root,)
    for base in bases:
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if (path.suffix in SOURCE_SUFFIXES or path.name == "Kconfig.projbuild") and not any(part in SKIP for part in path.parts):
                yield path


def _without_comments_or_strings(text):
    """`text` with every C/C++ comment and string/char literal blanked to
    spaces (newlines kept, so line numbers and `^`-anchored regexes still
    line up). C syntax only - `#`, `//` as division, `'` inside a word and a
    triple-quoted docstring all parse wrong under it, so `names()` below
    applies this to C_SUFFIXES only, never to a .py file.

    A name spelled `name()` only inside a comment or a message string - a
    citation of some OTHER function, say - is not a declaration or a call,
    so it must not count as the name being defined. Without this, a comment
    that itself cites a dead name (a stale "replaces old_name()") makes
    old_name() look real to every later scan, and a trim that garbles a
    cited name can point at nothing forever without this gate ever noticing.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:end]))
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" and j + 1 < n else 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def names(root):
    """Return function-like and macro/configuration names found in source."""
    functions, macros = set(), set()
    for path in source_paths(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        code = _without_comments_or_strings(text) if path.suffix in C_SUFFIXES else text
        functions.update(FUNCTION.findall(code))
        macros.update(MACRO.findall(text))
        macros.update(UPPERCASE.findall(text))
        macros.update("CONFIG_" + name for name in KCONFIG.findall(text))
    return functions, macros
