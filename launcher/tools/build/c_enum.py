"""Read C enum members and asset tag defines."""
import ast
import re
from pathlib import Path

from build.c_header import LITERAL_SUFFIX, read_define as read_literal_define
from build.math_constants import INTEGER_DEFINE, blank_comments

ASSET_TAG_WIDTH = 4
ASSET_TYPE_LITERAL = re.compile(r"ASSET_TYPE\s*(\(.*\))")


def tag_text(value: int) -> str:
    """The four bytes of an asset tag in storage order, as text."""
    return value.to_bytes(ASSET_TAG_WIDTH, "little").decode("latin1")


def tag_value(text: str) -> int:
    """The stored integer for a four-character asset tag."""
    if len(text) != ASSET_TAG_WIDTH:
        raise ValueError("an asset tag needs four characters")
    return int.from_bytes(text.encode("latin1"), "little")


def read_define(header, name):
    """Read an ASSET_TYPE define or delegate to the literal reader."""
    text = blank_comments(Path(header).read_text(encoding="utf-8"))
    value = next((value for define, value in INTEGER_DEFINE.findall(text) if define == name), "")
    asset = ASSET_TYPE_LITERAL.fullmatch(value.strip())
    if asset is None:
        return read_literal_define(header, name)
    try:
        chars = ast.literal_eval(asset[1])
        if isinstance(chars, tuple) and all(isinstance(c, str) and len(c) == 1 and c.isascii() for c in chars):
            return tag_value("".join(chars))
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
