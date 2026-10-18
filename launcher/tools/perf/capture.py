"""Shared identity checks for device measurement captures."""
import re
from collections.abc import Mapping
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/device"))
from device_lock import board_label, normalise_board
from device_report import find_manifest_entry

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build.c_header import read_define


def build_ids(text) -> list:
    """Every id a capture names, in `BUILD_ID=<id>` or `BUILD_ID <id>` form, wherever it sits in a line."""
    return re.findall(r"BUILD_ID[= ]([\w.-]+)", text)


def build_id(text) -> str:
    lines = [line for line in text.splitlines() if line.startswith("BUILD_ID")]
    if not lines:
        raise ValueError("missing build id")
    if len(lines) != 1 or not re.fullmatch(r"BUILD_ID=[\w.-]+", lines[0]):
        raise ValueError("invalid or repeated build id")
    return lines[0].removeprefix("BUILD_ID=")


def capture_board(path) -> str | None:
    """Read provenance from the records index owning this capture's folder."""
    path = Path(path).resolve()
    for folder in path.parents:
        index = folder / "index.jsonl"
        if index.is_file():
            entry = find_manifest_entry(path, index) or {}
            serial = entry.get("board")
            return normalise_board(serial) if serial else None
    return None


def require_same_board(boards: Mapping[str, str | None]) -> str | None:
    """Refuse pooled captures from distinct USB serials, including unknown provenance."""
    groups = {}
    for label, board in boards.items():
        groups.setdefault(board, []).append(str(label))
    if len(groups) > 1:
        details = [f"{board_label(board)}: {', '.join(paths)}" if board
                   else f"unknown: {', '.join(paths)}"
                   for board, paths in sorted(groups.items(), key=lambda pair: pair[0] or "")]
        raise ValueError("cannot compare captures from different boards: " + "; ".join(details))
    return next(iter(groups), None)


QUARTER_HEADER = Path(__file__).resolve().parents[2] / "main/display/display.h"
QUARTERS = tuple(read_define(QUARTER_HEADER, "DISPLAY_" + name + "_NAME")
                 for name in ("PORTRAIT", "LANDSCAPE"))
QUARTER_RE = re.compile(r"\bquarter=(\S+)")


def quarter_name(line):
    """Read an explicit quarter; untagged captures keep their existing identity."""
    match = QUARTER_RE.search(line)
    if match and match[1] not in QUARTERS:
        raise ValueError(f"invalid render quarter {match[1]}")
    return match[1] if match else None


def quarter_row(name, line):
    quarter = quarter_name(line)
    return f"{name} quarter={quarter}" if quarter else name


def capture_quarters(text, expected=()):
    """Validate tagged quarters and require any quarters the caller expects."""
    observed = {name for line in text.splitlines() if (name := quarter_name(line))}
    missing = set(expected) - observed
    if missing:
        raise ValueError("capture missing render quarter " + ", ".join(sorted(missing)))
    return observed


def select_quarter(text, quarter="portrait", observed=None):
    """Keep shared metadata and one tagged viewport without changing row identity."""
    if quarter not in QUARTERS:
        raise ValueError(f"invalid render quarter {quarter}")
    if observed is None:
        observed = capture_quarters(text)
    if (observed or quarter != "portrait") and quarter not in observed:
        raise ValueError(f"capture has no quarter={quarter} rows")
    return "\n".join(line for line in text.splitlines()
                     if quarter_name(line) in (None, quarter))
