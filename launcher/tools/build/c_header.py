"""Read decimal integer constants from their owning C header."""

import re
from pathlib import Path


def read_define(header, name):
    """Read a decimal integer define from `header`, naming its path on failure."""
    header = Path(header)
    text = header.read_text(encoding="utf-8")
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+\(?(\d+)\)?", text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"{header} defines no {name}")
    return int(match[1])
