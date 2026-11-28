"""Join frame-profile captures with the fitted playback scenarios and their source budgets."""
import pathlib
import re
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[2]
MAIN = TOOLS.parent / "main"
APP = MAIN / "apps/render_lab"
sys.path[:0] = [str(TOOLS), str(TOOLS / "perf")]

from build.c_header import read_define
from capture import QUARTERS, read_capture_text
from frame_profile import first_frame_heaps, judged, profiles

SUITE = APP / "tests/suite_render_lab_frame_profile.c"
ROW_KEY = re.compile(r'SCENE_BAKED_ENTRY\("[^"]+",\s*"([^"]+)"')
BUDGET = re.compile(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}")
BOARD = re.compile(r"^BOARD=(\S+)", re.MULTILINE)
TRACK_BOARD = "A308"


def suite_rows(scene):
    """Timing goals and ceilings in the order of lab_perf_quarters."""
    source = SUITE.read_text(encoding="utf-8")
    scenes = (APP / "scene_baked.c").read_text(encoding="utf-8")
    entry = re.search(rf"\bscene_{re.escape(scene)}\s*=\s*{ROW_KEY.pattern}", scenes)
    key = entry[1] if entry else scene
    rows = []
    scenarios = re.findall(rf"^\s*\{{[^{{}}\n]*&scene_{re.escape(scene)}\b[^\n]+", source, re.MULTILINE)
    for declaration in scenarios:
        row = key
        if re.search(r"\btrue\s*,", declaration):
            row += "-" + read_define(SUITE, "TEXTURED_TAKE")
        for quarter, (mean, p95) in zip(QUARTERS, BUDGET.findall(declaration), strict=True):
            rows.append(dict(row=row, quarter=quarter, mean=int(mean), p95=int(p95)))
    return rows


def labeled_capture(label, path):
    """Read a plain or gzip log using the device capture reader."""
    rows = capture_rows(label, read_capture_text(path))
    if not rows:
        raise ValueError(f"{label}: no frame profiles in {path}")
    return rows


def capture_rows(label, text):
    """The judged playback and matching first-frame heap for each row and quarter."""
    board = BOARD.search(text)
    if board and board[1] != TRACK_BOARD:
        raise ValueError(f"{label}: expected board {TRACK_BOARD}, capture names {board[1]}")
    text = judged(text)
    heaps = {(row["row"], row["quarter"]): {"free": row["free"], "largest": row["largest"]}
             for row in first_frame_heaps(text)}
    return [dict(row, label=label, heap=heaps.get((row["row"], row["quarter"]))) for row in profiles(text)]


def scene_captures(readings, ceilings, scene, variant):
    """Canonicalize fitted-variant capture keys to the suite's scene keys."""
    fitted = variant.replace("_", "-")
    rows = {row["row"] for row in ceilings}
    aliases = {fitted + row.removeprefix(scene): row for row in rows if row.startswith(scene)}
    return [dict(reading, row=aliases.get(reading["row"], reading["row"]))
            for reading in readings if reading["row"] in rows or reading["row"] in aliases]
