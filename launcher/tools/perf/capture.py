"""Shared identity checks for device measurement captures."""
import re
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/device"))
from device import records_root, short_id
from device_lock import normalise_board
from device_report import find_manifest_entry


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


def require_same_board(captures):
    """Refuse pooled captures from distinct USB serials, including unknown provenance."""
    groups = {}
    missing = object()

    def collect(item, inherited=missing):
        if isinstance(item, (str, Path)):
            item = {"capture": str(item)}
        board = item.get("board", inherited)
        nested = item.get("suites")
        if nested is not None:
            for suite in nested.values():
                for run in suite["runs"]:
                    collect(run, board)
            return
        paths = item.get("captures") or [item.get("capture") or item.get("capture_path")]
        for raw in paths:
            if isinstance(raw, dict):
                collect(raw, board)
                continue
            path = Path(raw) if raw is not None else None
            serial = board
            if serial is missing:
                entry = {}
                if path is not None:
                    entry = (find_manifest_entry(path, path.parent / "index.jsonl")
                             or find_manifest_entry(path, records_root() / "index.jsonl") or {})
                serial = entry.get("board")
            serial = normalise_board(serial) if serial else None
            groups.setdefault(serial, []).append(str(raw) if raw is not None else item.get("label", "unnamed"))

    for capture in captures:
        collect(capture)
    if len(groups) > 1:
        details = [f"{short_id(board)} ({board}): {', '.join(paths)}" if board
                   else f"unknown: {', '.join(paths)}"
                   for board, paths in sorted(groups.items(), key=lambda pair: pair[0] or "")]
        raise ValueError("cannot compare captures from different boards: " + "; ".join(details))
    return next(iter(groups), None)
