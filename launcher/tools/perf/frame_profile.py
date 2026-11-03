"""Parse playback profiles and fixed-pose frame costs from device captures."""
import pathlib
import re

from build.c_header import read_define

HEADER = pathlib.Path(__file__).resolve().parents[2] / "test/frame_profile.h"
TOKENS = {name: re.escape(read_define(HEADER, "FRAME_PROFILE_" + name))
          for name in ("PREFIX", "FRAMES", "MEAN", "MED", "P95", "WORST", "BEST", "FPS", "LOW", "QUARTER")}
PROFILE_RE = re.compile(
    rf"{TOKENS['PREFIX']} (?P<row>\S+) {TOKENS['FRAMES']}(?P<frames>\d+) "
    + " ".join(rf"{TOKENS[name.upper()]} (?P<{name}>\d+)us" for name in ("mean", "med", "p95", "worst", "best"))
    + rf" \| {TOKENS['FPS']} {TOKENS['MEAN']} (?P<fps_mean>[\d.]+) "
      rf"{TOKENS['LOW']} (?P<fps_low>[\d.]+) {TOKENS['WORST']} (?P<fps_worst>[\d.]+) "
      rf"{TOKENS['QUARTER']}(?P<quarter>\w+)")
QUARTER_FORMAT = " " + read_define(HEADER, "FRAME_PROFILE_QUARTER") + "%s"


def _log_pattern(name, fields):
    parts = re.split(r"%[\d.]*(?:ll)?[sudf]", read_define(HEADER, name) + QUARTER_FORMAT)
    if len(parts) != len(fields) + 1:
        raise ValueError(f"{name}: format fields differ from parser")
    return "".join(re.escape(part) + rf"\s*(?P<{field}>{pattern})"
                   for part, (field, pattern) in zip(parts, fields)) + re.escape(parts[-1])


SUMMARY_RE = re.compile(_log_pattern("FRAME_PROFILE_ROW", (
    ("row", r"\S+"), ("mean", r"\d+"), ("fps_mean", r"[\d.]+"),
    *((name, r"\d+") for name in ("worst", "med", "p95")), ("quarter", r"\w+"))))
POSE_RE = re.compile(_log_pattern("FRAME_PROFILE_POSE", (
    ("row", r"\S+"), *((name, r"\d+") for name in ("time_ms", "clusters", "triangles", "frame_us")),
    ("quarter", r"\w+"))).replace(")ms", ")(?P<time_unit>ms|s)", 1))
TRACE_RE = re.compile(_log_pattern("FRAME_PROFILE_TRACE", (
    ("row", r"\S+"), ("n", r"\d+"), ("frames", r"[ \d:]*?"), ("quarter", r"\w+"))))
TRACE_FRAME = re.compile(r"(\d+):(\d+)")
PEAK_RE = re.compile(_log_pattern("FRAME_PROFILE_PEAK", (
    ("row", r"\S+"), ("rank", r"\d+"), ("t_ms", r"\d+"), ("frame_us", r"\d+"), ("prominence_us", r"\d+"),
    ("quarter", r"\w+"))))
REPLAY_RE = re.compile(_log_pattern("FRAME_PROFILE_REPLAY", (
    ("row", r"\S+"), ("t_ms", r"\d+"), ("counts", r"[^|\n]*?"), ("report", r"[^\n]*?"), ("quarter", r"\w+"))))
PATH_POSES = read_define(HEADER, "FRAME_PROFILE_PATH_POSES")
PEAK_PERCENT = read_define(HEADER, "FRAME_PROFILE_PEAK_PERCENT")
COUNT = re.compile(r"(\w+)=(-?\d+)")


def _records(pattern, text):
    return [{key: value if key in ("row", "quarter", "time_unit") else float(value) if key.startswith("fps_") else int(value)
             for key, value in match.groupdict().items()} for match in pattern.finditer(text)]


def judged(text):
    """The capture without a test's second run: a test whose first run left the heap lower
    plays again for the heap alone, logging its row and what follows it a second time."""
    starts = [text.rfind("\n", 0, match.start()) + 1 for match in PROFILE_RE.finditer(text)]
    keys = [(match["row"], match["quarter"]) for match in PROFILE_RE.finditer(text)]
    kept, seen = [text[:starts[0]] if starts else text], set()
    for start, end, key in zip(starts, starts[1:] + [len(text)], keys):
        if key not in seen:
            seen.add(key)
            kept.append(text[start:end])
    return "".join(kept)


def profiles(text):
    return _records(PROFILE_RE, text)


def summaries(text):
    return _records(SUMMARY_RE, text)


def poses(text):
    from frame_cost import MS_PER_SECOND
    rows = _records(POSE_RE, text)
    for row in rows:
        if row.pop("time_unit") == "s":
            row["time_ms"] *= MS_PER_SECOND
    return rows


def trace(text):
    """Every played frame: row, quarter, its path time and its wall time. Fails on a line cut in
    transfer, and on a row whose traced frames differ from its profile's count of them."""
    frames, traced = [], {}
    for match in TRACE_RE.finditer(text):
        pairs = TRACE_FRAME.findall(match["frames"])
        key = (match["row"], match["quarter"])
        if len(pairs) != int(match["n"]):
            raise ValueError(f"{' '.join(key)}: a trace line holds {len(pairs)} of its {match['n']} frames")
        traced[key] = traced.get(key, 0) + len(pairs)
        frames.extend({"row": match["row"], "quarter": match["quarter"], "t_ms": int(t), "frame_us": int(us)}
                      for t, us in pairs)
    played = {(row["row"], row["quarter"]): row["frames"] for row in profiles(text)}
    for key, count in traced.items():
        if played.get(key) != count:
            raise ValueError(f"{' '.join(key)}: {count} frames traced of the {played.get(key)} played")
    return frames


def peaks(text):
    """The frame peaks each row and quarter names, rank 1 the highest."""
    return _records(PEAK_RE, text)


def replays(text):
    """Each replayed pose: row, quarter, path time, the cull view's counts and the
    frame-cost report's {bracket: mean ms} over its frames."""
    from frame_cost import REPORT_PREFIX, frame_cost_window
    rows = []
    for match in REPLAY_RE.finditer(text):
        window = frame_cost_window(f"{REPORT_PREFIX} {match['report']}")
        rows.append({"row": match["row"], "quarter": match["quarter"], "t_ms": int(match["t_ms"]),
                     "counts": {name: int(value) for name, value in COUNT.findall(match["counts"])},
                     "stages": {name: avg for name, (avg, _) in window["brackets"].items()}})
    return rows
