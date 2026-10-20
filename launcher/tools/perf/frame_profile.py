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
LOG_HEADER = HEADER.parents[1] / "main/apps/render_lab/tests/sponza_perf_log.h"
QUARTER_FORMAT = read_define(LOG_HEADER.with_name("r3d_perf_quarters.h"), "R3D_PERF_QUARTER_FORMAT")


def _log_pattern(name, fields):
    parts = re.split(r"%[\d.]*(?:ll)?[sudf]", read_define(LOG_HEADER, name) + QUARTER_FORMAT)
    if len(parts) != len(fields) + 1:
        raise ValueError(f"{name}: format fields differ from parser")
    return "".join(re.escape(part) + rf"\s*(?P<{field}>{pattern})"
                   for part, (field, pattern) in zip(parts, fields)) + re.escape(parts[-1])


SUMMARY_RE = re.compile(_log_pattern("SPONZA_PERF_SUMMARY", (
    ("row", r"\S+"), ("mean", r"\d+"), ("fps_mean", r"[\d.]+"),
    *((name, r"\d+") for name in ("worst", "med", "p95")), ("quarter", r"\w+"))))
POSE_RE = re.compile(_log_pattern("SPONZA_PERF_POSE_FORMAT", (
    ("row", r"\S+"), *((name, r"\d+") for name in ("time_ms", "clusters", "triangles", "frame_us")),
    ("quarter", r"\w+"))).replace(")ms", ")(?P<time_unit>ms|s)", 1))


def _records(pattern, text):
    return [{key: value if key in ("row", "quarter", "time_unit") else float(value) if key.startswith("fps_") else int(value)
             for key, value in match.groupdict().items()} for match in pattern.finditer(text)]


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
