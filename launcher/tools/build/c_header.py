"""Read literal and named math constants from their owning C header."""

import ast
import re
from pathlib import Path

from build.math_constants import MATH_HEADERS, math_constant_table


LITERAL_SUFFIX = re.compile(
    r"\b(0[xX][0-9a-fA-F]+)[uUlL]+\b|(?<![\w.])([0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?)[fFuUlL]+\b")


def read_define(header, name):
    """Read a numeric, string, brace-vector literal or named math call, naming its path on failure."""
    header = Path(header)
    text = header.read_text(encoding="utf-8")
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+([^\n]+)", text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"{header} defines no {name}")
    string = re.match(r'\(*\s*("(?:[^"\\]|\\.)*")', match[1].strip())
    if string:
        value = string[1]
    else:
        value = re.split(r"/\*|//", match[1])[0].strip()
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
