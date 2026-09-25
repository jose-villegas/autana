"""Names declared by the engine and its checked-in maintenance scripts."""
import pathlib
import re

SKIP = {"build", "build.dev", "build.diag", "build.qemu", "build.qemu.perf", "build.qemu.shell", "managed_components", ".git"}
SOURCE_SUFFIXES = {".c", ".h", ".py"}
C_SUFFIXES = {".c", ".h"}
FUNCTION = re.compile(r"\b([a-z_][a-z0-9_]*)\s*\(")
MACRO = re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]+)\b", re.M)
KCONFIG = re.compile(r"^\s*config\s+([A-Z][A-Z0-9_]+)\b", re.M)
# A constant as a comment cites one: at least one underscore, so a shouted
# word ("NOT", "THE DEFECT") never reads as a citation.
CONSTANT = re.compile(r"\b([A-Z][A-Z0-9]*_[A-Z0-9_]*[A-Z0-9])\b")
IDENTIFIER = re.compile(r"\b([A-Za-z_]\w*)\b")
PY_COMMENT = re.compile(r"^\s*#.*$", re.M)
PY_CONSTANT = re.compile(r"^([A-Z][A-Z0-9_]+)\s*=", re.M)
SDKCONFIG = re.compile(r"^#?\s*(CONFIG_[A-Z0-9_]+)", re.M)


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


def _without_comments_or_strings(text, strings=True):
    """`text` with every C/C++ comment and string/char literal blanked to
    spaces (newlines kept, so line numbers and `^`-anchored regexes still
    line up). C syntax only - `#`, `//` as division, `'` inside a word and a
    triple-quoted docstring all parse wrong under it, so `vocabulary()` below
    applies this to C_SUFFIXES only, never to a .py file.

    A name spelled `name()` only inside a comment or a message string - a
    citation of some OTHER function, say - is not a declaration or a call,
    so it must not count as the name being defined. Without this, a comment
    that itself cites a dead name (a stale "replaces old_name()") makes
    old_name() look real to every later scan, and a trim that garbles a
    cited name can point at nothing forever without this gate ever noticing.
    `strings=False` keeps literals: a protocol token the code prints
    ("TUNE_OK") is real vocabulary even though no code names it.
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
        elif strings and text[i] in "\"'":
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


class Vocabulary:
    """What a comment can cite, split by where the name is real.

    `functions`: declared or called in C code.

    `script_functions`: defined or called only by a Python script, so a C
    comment citing one must name the script.

    `constants`: spelled in C code or a string literal, #defined, a Kconfig
    option (with or without CONFIG_), an sdkconfig default, or anywhere in a
    script outside its `#` comments - never a name only a comment spells.

    `families`: the first word of every C #define, and CONFIG_ plus the
    first word of every project Kconfig option; a cited constant outside
    them (ESP_FAIL, CONFIG_PM_ENABLE) belongs to the SDK and is not checked.

    `defined_in`: file name -> every identifier its code spells, for a
    citation that pins a name to a file.
    """

    def __init__(self):
        self.functions = set()
        self.script_functions = set()
        self.constants = set()
        self.families = set()
        self.defined_in = {}


def vocabulary(root):
    """The Vocabulary of every source under `root` (see source_paths())."""
    vocab = Vocabulary()
    root = pathlib.Path(root)
    kconfig = []
    for path in source_paths(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        if path.suffix in C_SUFFIXES:
            code = _without_comments_or_strings(text)
            # "\nSELFTEST_COMPLETE": an escape must not glue a letter onto the name.
            spelled = re.sub(r"\\[a-z]", " ", _without_comments_or_strings(text, strings=False))
            functions = set(FUNCTION.findall(code))
            constants = set(CONSTANT.findall(spelled))
            vocab.functions |= functions
            vocab.constants |= constants
            vocab.families |= {name.split("_", 1)[0] for name in MACRO.findall(code)}
            vocab.defined_in.setdefault(path.name, set()).update(IDENTIFIER.findall(code))
        elif path.suffix == ".py":
            vocab.script_functions |= set(FUNCTION.findall(text))
            # Top-level assignments, and names a script only spells in a
            # string - an environment variable it reads, a line it matches.
            vocab.constants |= set(PY_CONSTANT.findall(text))
            vocab.constants |= set(CONSTANT.findall(PY_COMMENT.sub("", text)))
        else:
            kconfig += KCONFIG.findall(text)
    vocab.script_functions -= vocab.functions
    vocab.constants |= {"CONFIG_" + name for name in kconfig} | set(kconfig)
    vocab.families |= {"CONFIG_" + name.split("_", 1)[0] for name in kconfig}
    for path in sorted(root.rglob("sdkconfig.defaults*")):
        if not any(part in SKIP for part in path.parts):
            vocab.constants |= set(SDKCONFIG.findall(path.read_text(encoding="utf-8", errors="replace")))
    return vocab


def family(name):
    """The family a constant belongs to: CONFIG_ plus the option's first
    word for a configuration option, otherwise its own first word."""
    if name.startswith("CONFIG_"):
        return "CONFIG_" + name[len("CONFIG_"):].split("_", 1)[0]
    return name.split("_", 1)[0]
