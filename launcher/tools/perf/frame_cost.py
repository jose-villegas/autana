"""Parse the shell frame-cost report windows and their bracket means."""

import pathlib
import re
import statistics

from capture import quarter_row

REPORT_PREFIX = "ms/frame avg/worst:"
MS_PER_SECOND = 1000
US_PER_MS = 1000
US_PER_SECOND = US_PER_MS * MS_PER_SECOND
BRACKET = re.compile(r"(\S+) ([\d.]+)/([\d.]+)")
TOTAL = re.compile(r"\| total ([\d.]+)")
EXTERNAL = re.compile(r"(\S+) ([\d.]+)/([\d.]+) ms/block n=(\d+)")


def frame_cost_window(line):
    if REPORT_PREFIX not in line:
        return None
    frame, *external = line.split(REPORT_PREFIX, 1)[1].split("||")
    total = TOTAL.search(frame)
    return {"brackets": {name: (float(avg), float(worst)) for name, avg, worst in BRACKET.findall(frame)},
            "total": float(total[1]) if total else None,
            "external": {name: (float(avg), float(worst), int(n))
                         for section in external for name, avg, worst, n in EXTERNAL.findall(section)}}


def frame_cost_windows(path, text=None):
    if text is None:
        if path is None or not pathlib.Path(path).exists():
            return None
        text = pathlib.Path(path).read_text(encoding="utf-8", errors="replace")
    windows = [[(quarter_row(name, line), str(values[0])) for name, values in window["brackets"].items()]
               for line in text.splitlines() if (window := frame_cost_window(line)) is not None]
    if not windows or not all(windows):
        raise ValueError(f"{path}: no frame-cost windows")
    return windows


def frame_cost_means(path, text=None):
    """Every bracket in report order, averaged over its report windows."""
    windows = frame_cost_windows(path, text)
    if windows is None:
        return None
    values = {}
    for window in windows:
        for name, value in window:
            values.setdefault(name, []).append(float(value))
    return [(name, statistics.mean(samples)) for name, samples in values.items()]
