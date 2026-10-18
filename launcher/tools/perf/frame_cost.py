"""Parse the shell frame-cost report windows and their bracket means."""

import pathlib
import re
import statistics

from capture import quarter_row

REPORT_PREFIX = "ms/frame avg/worst:"
BRACKET = re.compile(r"(\S+) ([\d.]+)/[\d.]+")


def frame_cost_windows(path, text=None):
    if text is None:
        if path is None or not pathlib.Path(path).exists():
            return None
        text = pathlib.Path(path).read_text(encoding="utf-8", errors="replace")
    windows = [[(quarter_row(name, line), value) for name, value in BRACKET.findall(line.split(REPORT_PREFIX, 1)[1])]
               for line in text.splitlines()
               if REPORT_PREFIX in line]
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
