"""Parse the shell frame-cost report windows and their bracket means, and
the lap windows a --frame-laps image reports beside them."""

import pathlib
import re
import statistics

from build.c_header import read_define
from capture import quarter_row
from device_report import read_capture_text

LAUNCHER = pathlib.Path(__file__).resolve().parents[2]

REPORT_PREFIX = "ms/frame avg/worst:"
MS_PER_SECOND = 1000
US_PER_MS = 1000
US_PER_SECOND = US_PER_MS * MS_PER_SECOND
BRACKET = re.compile(r"(\S+) ([\d.]+)/([\d.]+)")
TOTAL = re.compile(r"\| total ([\d.]+)")
DROPPED = re.compile(r"\+(\d+) dropped")  # brackets or laps a report refused a slot
EXTERNAL = re.compile(r"(\S+) ([\d.]+)/([\d.]+) ms/block n=(\d+)")
LAPS_PREFIX = read_define(LAUNCHER / "main/profile/frame_laps.h", "FRAME_LAPS_PREFIX")
# A frame profile take's lap line: its row, then the shell's lap line (launcher/test/frame_profile.h).
LAPS_TAKE = re.compile(re.escape(read_define(LAUNCHER / "test/frame_profile.h", "FRAME_PROFILE_LAPS"))
                       .replace("%s", r"(\S+)") + re.escape(LAPS_PREFIX))
LAP_COST = re.compile(r"lap (\d+) ns")
LAP_CORE = re.compile(r"cpu(\d+)((?: \S+ [\d.]+/\d+)+)")
LAP = re.compile(r"(\S+) ([\d.]+)/(\d+)")
NS_PER_MS = 1_000_000


def frame_cost_window(line):
    """One report line's brackets, total and external stages; None for another
    line. A window that dropped brackets is refused: its brackets are not the
    whole frame, so no reader may average it."""
    if REPORT_PREFIX not in line:
        return None
    frame, *external = line.split(REPORT_PREFIX, 1)[1].split("||")
    total = TOTAL.search(frame)
    dropped = DROPPED.search(frame)
    if dropped:
        raise ValueError(f"a frame-cost window dropped {dropped[1]} brackets: {line.strip()}")
    return {"brackets": {name: (float(avg), float(worst)) for name, avg, worst in BRACKET.findall(frame)},
            "total": float(total[1]) if total else None,
            "external": {name: (float(avg), float(worst), int(n))
                         for section in external for name, avg, worst, n in EXTERNAL.findall(section)}}


def frame_cost_windows(path, text=None):
    if text is None:
        if path is None or not pathlib.Path(path).exists():
            return None
        text = read_capture_text(path)
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
    """{core: {lap: (ms, laps' own ms)}} per frame of one shell lap line, None
    for another line, a take's included: each lap's time with its starts' own
    cost taken out, and that cost apart. A window that dropped laps is
    refused, as a bracket window is."""
    if LAPS_PREFIX not in line or LAPS_TAKE.search(line):
        return None
    return lap_window(line)


def frame_laps_take(line):
    """(row with its quarter, window) of one frame profile take's lap line,
    None for another line."""
    take = LAPS_TAKE.search(line)
    return (quarter_row(take[1], line), lap_window(line)) if take else None


def lap_window(line):
    laps = line.split(LAPS_PREFIX, 1)[1]
    dropped = DROPPED.search(laps)
    if dropped:
        raise ValueError(f"a lap window dropped {dropped[1]} laps: {line.strip()}")
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
    text = read_capture_text(path)
    windows = [window for line in text.splitlines() if (window := frame_laps_window(line)) is not None]
    if not windows:
        raise ValueError(f"{path}: no lap windows")
    return laps_means(windows)


def frame_laps_takes(path):
    """{row with its quarter: frame_laps_means' shape} over a capture's frame
    profile takes, each row and quarter averaged apart."""
    text = read_capture_text(path)
    rows = {}
    for line in text.splitlines():
        if (take := frame_laps_take(line)) is not None:
            rows.setdefault(take[0], []).append(take[1])
    if not rows:
        raise ValueError(f"{path}: no take lap lines")
    return {row: laps_means(windows) for row, windows in rows.items()}


def laps_means(windows):
    names = {}
    for window in windows:
        for core, laps in window.items():
            names.setdefault(core, {}).update(dict.fromkeys(laps))
    return {core: {name: tuple(statistics.mean(window.get(core, {}).get(name, (0.0, 0.0))[part] for window in windows)
                               for part in range(2))
                   for name in laps}
            for core, laps in sorted(names.items())}
