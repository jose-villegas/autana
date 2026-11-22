"""Read C enum members and four-character asset tags."""
import ast
import re
from pathlib import Path

from build.c_header import LITERAL_SUFFIX
from build.math_constants import INTEGER_DEFINE, blank_comments

ASSET_TAG_WIDTH = 4
ASSET_TYPE_LITERAL = re.compile(r"ASSET_TYPE\s*(\(.*\))")


def read_asset_type(header, name, *, value=None):
    """Read an ASSET_TYPE define as the little-endian integer stored by C."""
    if value is None:
        text = blank_comments(Path(header).read_text(encoding="utf-8"))
        value = next((m[2] for m in INTEGER_DEFINE.finditer(text) if m[1] == name), None)
        if value is None:
            raise RuntimeError(f"{header} defines no {name}")
    asset = ASSET_TYPE_LITERAL.fullmatch(value.strip())
    if asset:
        try:
            chars = ast.literal_eval(asset[1])
            if (isinstance(chars, tuple) and len(chars) == ASSET_TAG_WIDTH
                    and all(isinstance(c, str) and len(c) == 1 for c in chars)):
                return int.from_bytes("".join(chars).encode("ascii"), "little")
        except (ValueError, SyntaxError):
            pass
    raise RuntimeError(f"{header}: {name} needs four ASCII asset type characters")


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
