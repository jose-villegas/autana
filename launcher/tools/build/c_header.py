"""Read literal constants from their owning C header."""

import ast
import re
from pathlib import Path


LITERAL_SUFFIX = re.compile(
    r"\b(0[xX][0-9a-fA-F]+)[uUlL]+\b|(?<![\w.])([0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?)[fFuUlL]+\b")


def read_define(header, name):
    """Read a numeric, string or brace-vector literal, naming its path on failure."""
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
        if "{" in value:
            value = "(" + value.split("{", 1)[1].split("}", 1)[0] + ",)"
        value = LITERAL_SUFFIX.sub(lambda match: match[1] or match[2], value)
    try:
        return ast.literal_eval(value)
    except (ValueError, SyntaxError) as error:
        raise RuntimeError(f"{header}: {name} is not a literal") from error
