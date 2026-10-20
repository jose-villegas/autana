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
SUMMARY_RE = re.compile(
    r"(?P<row>\S+) both cores: mean (?P<mean>\d+)us \((?P<fps_mean>[\d.]+) fps before present\), "
    r"worst (?P<worst>\d+)us, med (?P<med>\d+)us, p95 (?P<p95>\d+)us quarter=(?P<quarter>\w+)")
POSE_RE = re.compile(
    r"(?P<row>\S+) t=\s*(?P<time_ms>\d+)ms clusters=\s*(?P<clusters>\d+) tris=\s*(?P<triangles>\d+) "
    r"\| both cores: frame\s+(?P<frame_us>\d+)us quarter=(?P<quarter>\w+)")


def _records(pattern, text):
    return [{key: value if key in ("row", "quarter") else float(value) if key.startswith("fps_") else int(value)
             for key, value in match.groupdict().items()} for match in pattern.finditer(text)]


def profiles(text):
    return _records(PROFILE_RE, text)


def summaries(text):
    return _records(SUMMARY_RE, text)


def poses(text):
    return _records(POSE_RE, text)
