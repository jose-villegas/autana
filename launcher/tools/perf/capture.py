"""Shared identity checks for device measurement captures."""
import re


def build_id(text) -> str:
    lines = [line for line in text.splitlines() if line.startswith("BUILD_ID")]
    if not lines:
        raise ValueError("missing build id")
    if len(lines) != 1 or not re.fullmatch(r"BUILD_ID=[\w.-]+", lines[0]):
        raise ValueError("invalid or repeated build id")
    return lines[0].removeprefix("BUILD_ID=")
