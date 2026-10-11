"""Read integer and string constants from their owning C header."""

import ast
import re
from pathlib import Path


def read_define(header, name):
    """Read an integer or string define from `header`, naming its path on failure."""
    header = Path(header)
    text = header.read_text(encoding="utf-8")
    match = re.search(r"^#define\s+" + re.escape(name) + r'\s+\(?(\d+|"(?:[^"\\]|\\.)*")\)?', text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"{header} defines no {name}")
    return ast.literal_eval(match[1]) if match[1].startswith('"') else int(match[1])
