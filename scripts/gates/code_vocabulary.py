"""Names declared by the engine and its checked-in maintenance scripts."""
import pathlib
import re

from tracked import committable
from check_comment_length import blank_comments

SOURCE_SUFFIXES = {".c", ".h", ".py", ".sh", ".mjs"}
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
        for path in committable(base):
            if path.suffix in SOURCE_SUFFIXES or path.name == "Kconfig.projbuild":
                yield path



class Vocabulary:
    """What a comment can cite, split by where the name is real.

    `functions`: declared or called in C code.

    `script_functions`: defined or called only by a Python script, so a C
    comment citing one must name the script.

    `constants`: spelled in C code or a string literal, #defined, a Kconfig
    option (with or without CONFIG_), an sdkconfig default, or anywhere in a
    Python or shell script outside its `#` comments: never a name only a
    comment spells.

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
            code = blank_comments(text, strings=True)
            # "\nSELFTEST_COMPLETE": an escape must not glue a letter onto the name.
            spelled = re.sub(r"\\[a-z]", " ", blank_comments(text, literals=False))
            functions = set(FUNCTION.findall(code))
            constants = set(CONSTANT.findall(spelled))
            vocab.functions |= functions
            vocab.constants |= constants
            vocab.families |= {name.split("_", 1)[0] for name in MACRO.findall(code)}
            vocab.defined_in.setdefault(path.name, set()).update(IDENTIFIER.findall(code))
        elif path.suffix == ".py":
            vocab.script_functions |= set(FUNCTION.findall(text))
            # Top-level assignments, and names a script only spells in a
            # string; an environment variable it reads, a line it matches.
            vocab.constants |= set(PY_CONSTANT.findall(text))
            vocab.constants |= set(CONSTANT.findall(PY_COMMENT.sub("", text)))
        elif path.suffix == ".sh":
            # A variable a shell script sets or reads: a device profile's
            # value, an override the environment passes in.
            vocab.constants |= set(CONSTANT.findall(PY_COMMENT.sub("", text)))
        elif path.suffix == ".mjs":
            # An environment variable a Node gate reads; its functions are
            # camelCase and never cited as a C or Python name would be.
            vocab.constants |= set(CONSTANT.findall(blank_comments(text, literals=False)))
        else:
            kconfig += KCONFIG.findall(text)
    vocab.script_functions -= vocab.functions
    vocab.constants |= {"CONFIG_" + name for name in kconfig} | set(kconfig)
    vocab.families |= {"CONFIG_" + name.split("_", 1)[0] for name in kconfig}
    for path in committable(root):
        if path.name.startswith("sdkconfig.defaults"):
            vocab.constants |= set(SDKCONFIG.findall(path.read_text(encoding="utf-8", errors="replace")))
    return vocab


def family(name):
    """The family a constant belongs to: CONFIG_ plus the option's first
    word for a configuration option, otherwise its own first word."""
    if name.startswith("CONFIG_"):
        return "CONFIG_" + name[len("CONFIG_"):].split("_", 1)[0]
    return name.split("_", 1)[0]
