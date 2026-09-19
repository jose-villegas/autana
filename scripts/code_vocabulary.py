"""Names declared by the engine and its checked-in maintenance scripts."""
import pathlib
import re

SKIP = {"build", "build.dev", "build.diag", "build.qemu", "build.qemu.perf", "build.qemu.shell", "managed_components", ".git"}
SOURCE_SUFFIXES = {".c", ".h", ".py"}
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


def names(root):
    """Return function-like and macro/configuration names found in source."""
    functions, macros = set(), set()
    for path in source_paths(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        functions.update(FUNCTION.findall(text))
        macros.update(MACRO.findall(text))
        macros.update(UPPERCASE.findall(text))
        macros.update("CONFIG_" + name for name in KCONFIG.findall(text))
    return functions, macros
