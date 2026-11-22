"""Read C enum members."""
import re
from pathlib import Path

from build.c_header import LITERAL_SUFFIX
from build.math_constants import blank_comments


def read_enum(header, name=None):
    """Read a named typedef enum, or all typedef enum members with no name."""
    text = blank_comments(Path(header).read_text(encoding="utf-8"))
    enums = re.findall(r"\btypedef\s+enum\b[^{};]*\{([^{}]*)\}\s*(\w+)\s*;", text)
    if name is not None and all(enum != name for _, enum in enums):
        raise RuntimeError(f"{header} defines no enum {name}")
    members = {}
    for body, enum in enums:
        if name is not None and enum != name:
            continue
        value = -1
        for entry in body.split(","):
            member, _, given = (part.strip() for part in entry.partition("="))
            if member:
                value = int(LITERAL_SUFFIX.sub(r"\1\2", given), 0) if given else value + 1
                members[member] = value
    return members
