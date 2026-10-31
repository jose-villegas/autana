"""Parse the shell frame-cost report windows and their bracket means, and
the lap windows a --frame-laps image reports beside them."""

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
LAPS_PREFIX = "ms/frame laps:"
LAP_COST = re.compile(r"lap (\d+) ns")
LAP_CORE = re.compile(r"cpu(\d+)((?: \S+ [\d.]+/\d+)+)")
LAP = re.compile(r"(\S+) ([\d.]+)/(\d+)")
NS_PER_MS = 1_000_000


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


def frame_laps_window(line):
    """{core: {lap: (ms, laps' own ms)}} per frame of one lap line, None for
    another line: each lap's time with its starts' own cost taken out, and
    that cost apart."""
    if LAPS_PREFIX not in line:
        return None
    laps = line.split(LAPS_PREFIX, 1)[1]
    cost = LAP_COST.search(laps)
    if cost is None:
        raise ValueError(f"a lap line without its lap cost: {line.strip()}")
    lap_ms = int(cost[1]) / NS_PER_MS
    return {int(core): {name: (float(ms) - int(starts) * lap_ms, int(starts) * lap_ms)
                        for name, ms, starts in LAP.findall(names)}
            for core, names in LAP_CORE.findall(laps)}


def frame_laps_means(path):
    """{core: {lap: (mean ms, mean laps' own ms)}} per frame over every lap
    window, a lap missing from a window counting as 0, so a core's laps keep
    adding up."""
    text = pathlib.Path(path).read_text(encoding="utf-8", errors="replace")
    windows = [window for line in text.splitlines() if (window := frame_laps_window(line)) is not None]
    if not windows:
        raise ValueError(f"{path}: no lap windows")
    names = {}
    for window in windows:
        for core, laps in window.items():
            names.setdefault(core, {}).update(dict.fromkeys(laps))
    return {core: {name: tuple(statistics.mean(window.get(core, {}).get(name, (0.0, 0.0))[part] for window in windows)
                               for part in range(2))
                   for name in laps}
            for core, laps in sorted(names.items())}
