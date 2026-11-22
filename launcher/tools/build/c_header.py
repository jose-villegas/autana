"""Read literal and named math constants from their owning C header."""

import ast
import re
from pathlib import Path

from build.math_constants import MATH_HEADERS, blank_comments, math_constant_table


LITERAL_SUFFIX = re.compile(
    r"\b(0[xX][0-9a-fA-F]+)[uUlL]+\b|(?<![\w.])([0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?)[fFuUlL]+\b")
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
    """Read a literal, asset tag or named math call, naming its path on failure."""
    header = Path(header)
    text = blank_comments(header.read_text(encoding="utf-8"))
    match = re.search(r"^[ \t]*#[ \t]*define[ \t]+" + re.escape(name) + r"[ \t]+([^\n]+)", text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"{header} defines no {name}")
    string = re.match(r'\(*\s*("(?:[^"\\]|\\.)*")', match[1].strip())
    if string:
        value = string[1]
    else:
        value = match[1].strip()
        asset = ASSET_TYPE_LITERAL.fullmatch(value)
        if asset:
            try:
                chars = ast.literal_eval(asset[1])
                if isinstance(chars, tuple) and all(isinstance(c, str) and len(c) == 1 and c.isascii() for c in chars):
                    return tag_value("".join(chars))
            except (ValueError, SyntaxError):
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
