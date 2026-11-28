"""Join frame-profile captures with their logged heaps and budgets."""
import pathlib
import re
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(TOOLS), str(TOOLS / "perf")]

from capture import read_capture_text
from frame_profile import first_frame_heaps, judged, profiles
from perf.perf_targets import parse_perf_ceilings, parse_perf_targets

BOARD = re.compile(r"^BOARD=(\S+)", re.MULTILINE)
TRACK_BOARD = "A308"


def labeled_capture(label, path):
    """Read a plain or gzip log using the device capture reader."""
    rows = capture_rows(label, read_capture_text(path))
    if not rows:
        raise ValueError(f"{label}: no frame profiles in {path}")
    return rows


def capture_rows(label, text):
    """The judged playback and matching first-frame heap and budget for each row and quarter."""
    board = BOARD.search(text)
    if board and board[1] != TRACK_BOARD:
        raise ValueError(f"{label}: expected board {TRACK_BOARD}, capture names {board[1]}")
    text = judged(text)
    heaps = {(row["row"], row["quarter"]): {"free": row["free"], "largest": row["largest"]}
             for row in first_frame_heaps(text)}
    goals = {target["name"]: int(target["goal"]) for target in parse_perf_targets(text)[0]}
    ceilings = {guard["name"]: int(guard["ceiling"]) for guard in parse_perf_ceilings(text)}
    readings = []
    for row in profiles(text):
        key = row["row"], row["quarter"]
        name = " ".join(key)
        readings.append(dict(row, label=label, heap=heaps.get(key), goal=goals.get(name), ceiling=ceilings.get(name)))
    return readings


def scene_captures(readings, scene):
    """Select playback rows for the scene using its current and archived capture names."""
    # Archived captures can name the fitted variant, including when the suite profiles other variants alongside it.
    fitted = f"{scene}-fitted-full"
    rows = {scene: scene, f"{scene}-textures": f"{scene}-textures",
            fitted: scene, f"{fitted}-textures": f"{scene}-textures"}
    return [dict(reading, row=rows[reading["row"]]) for reading in readings if reading["row"] in rows]
