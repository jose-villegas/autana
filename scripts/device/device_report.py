"""Turns a raw device capture into a markdown report beside it, so a run's
result is readable without opening the serial log: suite PASS/FAIL counts,
every failing test's Unity message, any `PERF TARGET` lines, and - when
exactly one app's `tools/report_performance.py` matches the suite that ran -
that app's own frame-budget table.

Knows no app by name. A reporter is found by convention:
`launcher/main/apps/*/tools/report_performance.py`, paired with whichever
`suite_*.c` in that app's tests/ folder registers the suite that ran
(`SUITE_REGISTER(<suite>)` - the same name device.py's run-suite passed to
RUNSUITE). Zero matches, more than one, or a reporter that raises all fall
back to the generic summary with a stated reason; the capture is the
evidence, the per-app table is a convenience. See device.py's `report`
subcommand and its call from run_suite().
"""

import gzip
import json
import re
import subprocess
import sys
from pathlib import Path


# `\S*`, not report_performance.py's `\S+`: this project's own Unity result
# lines carry no filename ahead of the line number - "*:3685:name:PASS", not
# "file.c:3685:name:PASS" - so a required leading token never matches a real
# capture. Confirmed against 152333_runsuite-run_sand_perf_suite...log:
# report_performance.py's own RESULT_RE finds zero entries in it.
RESULT_RE = re.compile(r"^(?P<file>\S*?):\d+:(?P<name>\w+):(?P<status>PASS|FAIL)(?::\s*(?P<message>.*))?$")
# Searched, not anchored: a real line carries the ESP-IDF log prefix
# ("I (32139) device_tests: PERF TARGET full-size step: ..."), and a target's
# name contains spaces.
PERF_TARGET_RE = re.compile(
    r"PERF TARGET (?!SUMMARY:)(?P<name>.+?): measured (?P<measured>\d+) us, "
    r"goal (?P<goal>\d+) us, distance (?P<distance>[+-]?\d+(?:\.\d+)?%)")
PERF_SUMMARY_RE = re.compile(r"PERF TARGET SUMMARY:\s*(?P<unmet>\d+)\s*unmet")
# A measurement a perf test logs before its own result line, e.g.
# "I (32138) device_tests: sand_step on 184x224 with 10304 grains: 7159 us".
# The first number followed by "us" is the measurement: sizes and counts
# earlier on the line ("184x224", "10304 grains") are not.
DEVICE_TIMING_RE = re.compile(r"device_tests: .*?(?<![\w.-])(?P<us>\d+) us\b")
# "frame time, lava stress: sim 92767 us/frame" - one phase of a frame a test
# splits into sim, mark, present and total. A phase is never the test's own
# measurement, which is logged after it; a test logging only the split is
# measured by its total.
FRAME_TIME_PHASE_RE = re.compile(
    r"device_tests: frame time, .+?: (?P<phase>sim|mark|present|total) (?P<us>\d+) us/frame")

# The exact header report_performance.py emits for its one frame-budget
# table - see main()'s `lines.append` there. Matched verbatim rather than
# re-derived, so a wording change in that file breaks this loudly (a
# KeyError-shaped diff) instead of silently miscounting rows.
BUDGET_TABLE_HEADER = "| Test | Budget (us) | Measured (us) | Headroom | Status |"

REPORTER_TIMEOUT_SECONDS = 120


def read_capture_text(path):
    path = Path(path)
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as stream:
        return stream.read()


def read_index(index_path):
    try:
        text = Path(index_path).read_text(encoding="utf-8")
    except FileNotFoundError:
        return []
    entries = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            entries.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return entries


def find_manifest_entry(capture_path, index_path):
    """The last index.jsonl line naming this exact capture file - last, not
    first, since re-running a report over the same --out path would else
    always find the earliest run instead of the one that produced today's
    bytes."""
    capture_path = Path(capture_path).resolve()
    match = None
    for entry in read_index(index_path):
        raw = entry.get("capture_path")
        if raw and Path(raw).resolve() == capture_path:
            match = entry
    return match


def parse_suite_results(text):
    passed = failed = 0
    failures = []
    for line in text.splitlines():
        m = RESULT_RE.match(line.strip())
        if not m:
            continue
        if m.group("status") == "PASS":
            passed += 1
        else:
            failed += 1
            failures.append((m.group("name"), m.group("message") or ""))
    return passed, failed, failures


def parse_perf_targets(text):
    targets = []
    summary_unmet = None
    for line in text.splitlines():
        m = PERF_SUMMARY_RE.search(line)
        if m:
            summary_unmet = int(m.group("unmet"))
            continue
        m = PERF_TARGET_RE.search(line)
        if m:
            targets.append(m.groupdict())
    return targets, summary_unmet


def parse_timed_results(text):
    """test name -> (status, measurement in us or None, failure message).
    The measurement is the first timing a test logs before its own result
    line, a frame-time split aside; a test that logs none is a behaviour test
    and carries None."""
    rows = {}
    pending = None
    frame_total = None
    for line in text.splitlines():
        result = RESULT_RE.match(line.strip())
        if result:
            measured = pending if pending is not None else frame_total
            rows[result.group("name")] = (result.group("status"), measured,
                                          result.group("message") or "")
            pending = None
            frame_total = None
            continue
        if "PERF TARGET" in line:
            continue
        phase = FRAME_TIME_PHASE_RE.search(line)
        if phase:
            if phase.group("phase") == "total":
                frame_total = int(phase.group("us"))
            continue
        timing = DEVICE_TIMING_RE.search(line)
        if timing and pending is None:
            pending = int(timing.group("us"))
    return rows


def _row(per_run, run, name):
    return per_run.get(run, {}).get(name, (None, None, ""))


def batch_summary_markdown(entries, meta):
    """One report across a batch: every suite's timings side by side per run
    with the spread, the tests whose result CHANGED between runs of the same
    image (a test that flaps is a finding, not noise), the tests that failed
    in every run, and each target's measurement per run. `entries` is a list
    of {suite, run, capture, error}; `meta` carries build_id, owner, purpose,
    runs, worktree and commit."""
    runs = meta["runs"]
    run_ids = list(range(1, runs + 1))
    lines = ["# Device Batch Report", ""]
    for key in ("build_id", "worktree", "commit", "owner", "purpose"):
        lines.append(f"- {key.replace('_', ' ').title()}: `{meta.get(key)}`")
    lines += [f"- Runs per suite: {runs}", ""]
    suites = []
    for entry in entries:
        if entry["suite"] not in suites:
            suites.append(entry["suite"])
    for suite in suites:
        per_run, errors, targets = {}, [], {}
        for entry in (e for e in entries if e["suite"] == suite):
            if entry.get("error"):
                errors.append((entry["run"], entry["error"]))
            try:
                text = read_capture_text(entry["capture"])
            except OSError as problem:
                errors.append((entry["run"], f"capture unreadable: {problem}"))
                continue
            per_run[entry["run"]] = parse_timed_results(text)
            for target in parse_perf_targets(text)[0]:
                targets.setdefault(target["name"], {})[entry["run"]] = target
        names = []
        for rows in per_run.values():
            names += [name for name in rows if name not in names]
        lines += [f"## {suite}", ""]

        timed = [n for n in names if any(_row(per_run, r, n)[1] is not None for r in run_ids)]
        if timed:
            lines += ["### Timings (us)", "",
                      "| Test | " + " | ".join(f"run {r}" for r in run_ids) + " | min | max | spread |",
                      "|---|" + "---:|" * (len(run_ids) + 3)]
            for name in timed:
                values = [_row(per_run, r, name)[1] for r in run_ids]
                present = [v for v in values if v is not None]
                low, high = min(present), max(present)
                spread = f"{(high - low) * 100 / low:.1f}%" if low else "-"
                cells = " | ".join("-" if v is None else str(v) for v in values)
                lines.append(f"| `{name}` | {cells} | {low} | {high} | {spread} |")
            lines.append("")

        flapping, always_failed = [], []
        for name in names:
            statuses = [_row(per_run, r, name)[0] for r in run_ids]
            seen = {s for s in statuses if s}
            if len(seen) > 1:
                flapping.append((name, statuses))
            elif seen == {"FAIL"} and all(statuses):
                always_failed.append((name, _row(per_run, run_ids[0], name)[2]))
        lines += ["### Result changed between runs", ""]
        if flapping:
            lines += ["| Test | " + " | ".join(f"run {r}" for r in run_ids) + " |",
                      "|---|" + "---|" * len(run_ids)]
            for name, statuses in flapping:
                lines.append(f"| `{name}` | " + " | ".join(s or "-" for s in statuses) + " |")
        else:
            lines.append("_None: every test gave the same result in every run._")
        lines += ["", "### Failed in every run", ""]
        lines += [f"- `{name}`: {message}" for name, message in always_failed] or ["_None._"]
        lines.append("")
        if targets:
            lines += ["### Targets (us)", "",
                      "| Target | " + " | ".join(f"run {r}" for r in run_ids) + " | goal |",
                      "|---|" + "---:|" * (len(run_ids) + 1)]
            for name, by_run in targets.items():
                goal = next(iter(by_run.values()))["goal"]
                cells = " | ".join(by_run[r]["measured"] if r in by_run else "-" for r in run_ids)
                lines.append(f"| {name} | {cells} | {goal} |")
            lines.append("")
        if errors:
            lines += ["### Capture errors", ""]
            lines += [f"- run {run}: {message}" for run, message in errors]
            lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def looks_like_perf_capture(text):
    """True when the capture is plausibly a performance run, independent of
    whether any budget was ever declared for it - the signal the empty-table
    warning needs, since an empty table is only alarming when the capture
    itself clearly measured something."""
    return bool(re.search(r"^PERF TARGET ", text, re.MULTILINE)) or "device_tests" in text


def discover_reporters(worktree, suite):
    """Every app under `worktree` whose tools/report_performance.py exists
    AND whose own tests/suite_*.c registers `suite`. Reads only file names and
    grep-shaped text, never anything sand- or cube-specific, so a new app
    following the same convention needs no change here."""
    matches = []
    apps_root = Path(worktree) / "launcher" / "main" / "apps"
    if not apps_root.is_dir():
        return matches
    register_re = re.compile(r"SUITE_REGISTER\(\s*" + re.escape(suite) + r"\s*\)")
    for reporter_path in sorted(apps_root.glob("*/tools/report_performance.py")):
        app_dir = reporter_path.parents[1]
        for source in sorted((app_dir / "tests").glob("suite_*.c")):
            try:
                source_text = source.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if register_re.search(source_text):
                matches.append({"app": app_dir.name, "reporter": reporter_path, "source": source})
                break
    return matches


def run_reporter(reporter_path, capture_path, source_path, out_path, python_exe=None):
    python_exe = python_exe or sys.executable  # resolved per call, not bound at import
    command = [python_exe, str(reporter_path), str(capture_path), str(out_path),
               "--source", str(source_path)]
    return subprocess.run(command, capture_output=True, text=True,
                          timeout=REPORTER_TIMEOUT_SECONDS)


def count_budget_rows(report_markdown):
    lines = report_markdown.splitlines()
    try:
        start = lines.index(BUDGET_TABLE_HEADER)
    except ValueError:
        return 0
    rows = 0
    for line in lines[start + 2:]:
        if not line.startswith("|"):
            break
        rows += 1
    return rows


def _unavailable(reason):
    return ["## App Budget Table", "", "_Budget table unavailable: " + reason + "_", ""]


def build_budget_section(worktree, suite, capture_path, capture_text):
    if not worktree:
        return _unavailable("no worktree recorded for this capture, so no app's "
                            "reporter tooling can be located")
    if not suite:
        return _unavailable("this capture has no suite recorded (not a run-suite capture)")
    worktree_path = Path(worktree)
    if not worktree_path.is_dir():
        return _unavailable(f"recorded worktree `{worktree}` does not exist on this machine")

    matches = discover_reporters(worktree_path, suite)
    if not matches:
        return _unavailable(f"no app's `tools/report_performance.py` registers suite `{suite}`")
    if len(matches) > 1:
        apps = ", ".join(m["app"] for m in matches)
        return _unavailable(f"suite `{suite}` matched more than one app's reporter "
                            f"({apps}) - ambiguous, so none was run")

    match = matches[0]
    tmp_out = Path(capture_path).with_name(Path(capture_path).name + f".{match['app']}_budget.tmp.md")
    try:
        result = run_reporter(match["reporter"], capture_path, match["source"], tmp_out)
    except Exception as error:  # subprocess/timeout/OS error - a convenience, never fatal
        return _unavailable(f"running {match['app']}'s report_performance.py raised: {error}")

    if result.returncode != 0:
        tmp_out.unlink(missing_ok=True)
        stderr_tail = (result.stderr or result.stdout or "").strip().splitlines()
        detail = stderr_tail[-1] if stderr_tail else "no output"
        return _unavailable(f"{match['app']}'s report_performance.py exited "
                            f"{result.returncode}: {detail}")

    try:
        table_md = tmp_out.read_text(encoding="utf-8")
    finally:
        tmp_out.unlink(missing_ok=True)

    lines = ["## App Budget Table", "",
             f"### {match['app']} (`{match['reporter'].name}` against `{match['source'].name}`)",
             ""]
    if count_budget_rows(table_md) == 0 and looks_like_perf_capture(capture_text):
        lines.append(
            "**WARNING: the frame-budget table came back with zero rows, but this capture "
            "clearly contains performance output. report_performance.py matches "
            "`TEST_ASSERT_LESS_THAN_MESSAGE`; if the suite has moved to `perf_guard`/"
            "`perf_target`, the reporter is out of step with the suite - an engine-side fix, "
            "not a sign the run measured nothing.**")
        lines.append("")
    lines.append(table_md)
    return lines


def build_report_markdown(capture_path, index_path):
    capture_path = Path(capture_path)
    text = read_capture_text(capture_path)
    entry = find_manifest_entry(capture_path, index_path)

    lines = ["# Device Capture Report", "", f"- Capture: `{capture_path}`"]
    if entry:
        lines.append(f"- Suite: `{entry.get('suite') or 'n/a'}`")
        lines.append(f"- Build: `{entry.get('build_id') or 'unknown'}`")
        worktree = entry.get("worktree")
        commit = entry.get("commit")
        if worktree and commit:
            lines.append(f"- Worktree: `{worktree}` @ `{commit[:12]}`")
        elif worktree:
            lines.append(f"- Worktree: `{worktree}` @ unknown commit")
        elif commit:
            lines.append(f"- Commit: `{commit}` (worktree unknown)")
        else:
            lines.append("- Worktree: unknown")
        lines.append(f"- Owner: `{entry.get('owner') or 'unknown'}` - {entry.get('purpose') or 'n/a'}")
        ended = entry.get("reason") or "unknown"
        if entry.get("error"):
            ended += f" (error: {entry['error']})"
        lines.append(f"- Ended: {ended}")
    else:
        lines.append("- Manifest: no matching entry found in index.jsonl - "
                     "suite/build/worktree unknown")
    lines.append("")

    lines.append("## Suite Results")
    lines.append("")
    passed, failed, failures = parse_suite_results(text)
    if passed or failed:
        lines.append(f"PASS: {passed}  FAIL: {failed}")
        lines.append("")
        if failures:
            lines.append("### Failures")
            lines.append("")
            lines.append("| Test | Message |")
            lines.append("|---|---|")
            for name, message in failures:
                lines.append(f"| `{name}` | {message} |")
            lines.append("")
    else:
        lines.append("No `PASS`/`FAIL` result lines found in this capture.")
        lines.append("")

    targets, summary_unmet = parse_perf_targets(text)
    if targets or summary_unmet is not None:
        lines.append("## Performance Targets")
        lines.append("")
        lines.append("| Target | Measured (us) | Goal (us) | Distance |")
        lines.append("|---|---:|---:|---|")
        for target in targets:
            lines.append(f"| `{target['name']}` | {target['measured']} | {target['goal']} | "
                         f"{target['distance']} |")
        lines.append("")
        if summary_unmet is not None:
            lines.append(f"PERF TARGET SUMMARY: {summary_unmet} unmet")
            lines.append("")

    worktree = entry.get("worktree") if entry else None
    suite = entry.get("suite") if entry else None
    lines.extend(build_budget_section(worktree, suite, capture_path, text))

    return "\n".join(lines).rstrip("\n") + "\n"


def report_path_for(capture_path):
    capture_path = Path(capture_path)
    name = capture_path.name
    stem = name[: -len(".log.gz")] if name.endswith(".log.gz") else capture_path.stem
    return capture_path.with_name(stem + ".md")


def write_report_for_capture(capture_path, index_path):
    capture_path = Path(capture_path)
    markdown = build_report_markdown(capture_path, index_path)
    out_path = report_path_for(capture_path)
    out_path.write_text(markdown, encoding="utf-8")
    return out_path
