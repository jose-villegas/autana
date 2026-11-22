"""Read constants and enum values from their owning C header."""

import ast
import re
from pathlib import Path

from build.math_constants import INTEGER_DEFINE, MATH_HEADERS, blank_comments, math_constant_table


LITERAL_SUFFIX = re.compile(
    r"\b(0[xX][0-9a-fA-F]+)[uUlL]+\b|(?<![\w.])([0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?)[fFuUlL]+\b")


def read_define(header, name, *, text=None):
    """Read a literal, asset tag or named math call, naming its path on failure."""
    header = Path(header)
    if text is None:
        text = header.read_text(encoding="utf-8")
    match = next((m for m in INTEGER_DEFINE.finditer(text) if m[1] == name), None)
    if match is None:
        raise RuntimeError(f"{header} defines no {name}")
    string = re.match(r'\(*\s*("(?:[^"\\]|\\.)*")', match[2].strip())
    if string:
        value = string[1]
    else:
        value = blank_comments(match[2]).strip()
        asset = re.fullmatch(r"ASSET_TYPE\s*(\(.*\))", value)
        if asset:
            try:
                chars = ast.literal_eval(asset[1])
                if isinstance(chars, tuple) and len(chars) == 4 and all(isinstance(c, str) and len(c) == 1 for c in chars):
                    return int.from_bytes("".join(chars).encode("ascii"), "little")
            except (TypeError, ValueError, SyntaxError):
                pass
            raise RuntimeError(f"{header}: {name} needs four ASCII asset type characters")
        call = re.fullmatch(r"\(*\s*(\w+\(\))\s*\)*", value)
        if call:
            root = next((p for p in header.resolve().parents if (p / MATH_HEADERS).is_dir()), None)
            if root is not None:
                result = math_constant_table(root)[3].get(call[1])
                if result is not None:
                    return result
        if "{" in value:
            value = "(" + value.split("{", 1)[1].split("}", 1)[0] + ",)"
        value = LITERAL_SUFFIX.sub(lambda match: match[1] or match[2], value)
    try:
        return ast.literal_eval(value)
    except (ValueError, SyntaxError) as error:
        raise RuntimeError(f"{header}: {name} is not a literal") from error


def read_defines(header, prefix, names):
    """Read prefixed constants from their owning header in the requested order."""
    return [read_define(header, prefix + name) for name in names]


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
