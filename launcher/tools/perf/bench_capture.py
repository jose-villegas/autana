"""Parse package benchmark measurements and target provenance by record prefix."""
import re

from capture import build_id

NUMBER = r"[0-9]+(?:\.[0-9]+)?"


def parse_target(text, execution="hardware", *, prefix):
    target_row = re.compile(rf"{re.escape(prefix)} target=(\w+) arch=([\w-]+)(?: core=([\w.-]+))? execution=(hardware|qemu|host)")
    targets = set()
    for line in text.splitlines():
        if not line.startswith(prefix):
            continue
        match = target_row.fullmatch(line)
        if not match:
            raise ValueError(f"malformed benchmark target: {line}")
        targets.add(match.groups())
    if len(targets) > 1:
        raise ValueError("mixed benchmark capture targets")
    if not targets:
        return None
    target, architecture, core, observed = targets.pop()
    if observed != execution:
        raise ValueError(f"benchmark target measurements require {execution} execution")
    return f"{target} ({architecture}{', ' + core if core else ''})"


def parse_capture(text, rows=None, allow_missing=False, *, prefix, placeholder=None):
    if placeholder is not None and text.rstrip("\r\n") == placeholder:
        return None, {}
    provenance = [line for line in text.splitlines() if line.startswith("BUILD_ID")]
    if any("scope=" in line for line in provenance):
        scoped = [re.fullmatch(r"BUILD_ID=([\w.-]+) scope=(bench|error)", line) for line in provenance]
        if len(scoped) != 2 or any(match is None for match in scoped) or {match[2] for match in scoped} != {"bench", "error"}:
            raise ValueError("expected one bench and one error build id")
        build = build_id(next("BUILD_ID=" + match[1] for match in scoped if match[2] == "bench"))
    else:
        build = build_id(text)
    expected = {(label, kind): (key, ops) for key, label, kind, ops, family in rows} if rows is not None else None
    capture_row = re.compile(
        rf"{re.escape(prefix)} name=(.+) type=(\w+) ops=([1-9][0-9]*) warm=({NUMBER}) cold=({NUMBER}) insn=({NUMBER})")
    measured = {}
    for line in text.splitlines():
        if not line.startswith(prefix):
            continue
        match = capture_row.fullmatch(line)
        if match is None:
            raise ValueError(f"malformed measurement: {line}")
        label, kind, ops, *values = match.groups()
        if expected is not None and (label, kind) not in expected:
            raise ValueError(f"unexpected capture row: {label} {kind}")
        key, count = expected[label, kind] if expected is not None else ((label, kind), int(ops))
        if key in measured or int(ops) != count or any(float(value) <= 0 for value in values):
            raise ValueError(f"invalid or duplicate measurement: {line}")
        measured[key] = tuple(map(float, values))
    if rows is not None and not allow_missing and len(measured) != len(rows):
        raise ValueError("missing capture rows")
    return build, measured
