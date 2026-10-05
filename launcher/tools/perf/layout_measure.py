"""Seeded capture acquisition and variance estimates shared by performance tools."""
import json
import math
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from statistics import mean, stdev

MEAN_RE = re.compile(r"\b(?P<name>\S+) both cores: mean\s+(?P<value>\d+)us\b")
NUMBER_RE = re.compile(r"^[+-]?\d+$")
def cell(text):
    return text.strip().strip("`").replace("**", "")


def parse_markdown(text):
    lines = text.splitlines()
    collected = {}
    for index in range(len(lines) - 2):
        if not (lines[index].startswith("|") and lines[index + 1].startswith("|")):
            continue
        headers = [cell(part) for part in lines[index].strip("|").split("|")]
        if not all(re.fullmatch(r"\s*:?-{3,}:?\s*", part) for part in lines[index + 1].strip("|").split("|")):
            continue
        lowered = [header.lower() for header in headers]
        if not any(name in lowered[0] for name in ("test", "name", "scenario", "run", "mesh")):
            continue
        value_index = next((i for i, name in enumerate(lowered)
                            if "measured" in name or "mean" in name), None)
        if value_index is None and len(headers) == 2:
            value_index = 1
        if value_index is None:
            continue
        rows = {}
        for line in lines[index + 2:]:
            if not line.startswith("|"):
                break
            parts = [cell(part) for part in line.strip("|").split("|")]
            if len(parts) <= value_index or not NUMBER_RE.fullmatch(parts[value_index]):
                continue
            rows[parts[0]] = int(parts[value_index])
        collected.update(rows)
    return collected


def parse_report(path):
    """Return named microsecond rows from a report table or suite capture."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    rows = parse_markdown(text)
    if rows:
        return rows
    return {match.group("name"): int(match.group("value"))
            for match in MEAN_RE.finditer(text)}


REPO = Path(__file__).resolve().parents[3]
REPORT_LINE = re.compile(r"^report: (.+\.md)\s*$")
RUN_LINE = re.compile(r"^batch: (\S+) run (\d+)/(\d+)")
BUILD_LINE = re.compile(r"BUILD_ID=(\S+)")

# Two-sided 95% Student t by degrees of freedom; 1.96 past the table.
T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306,
       9: 2.262, 10: 2.228, 12: 2.179, 14: 2.145, 16: 2.120, 18: 2.101, 20: 2.086,
       25: 2.060, 30: 2.042, 40: 2.021, 60: 2.000, 120: 1.980}


def t95(degrees):
    """The two-sided 95% t quantile for `degrees` of freedom, rounding the
    degrees down to the nearest table entry, which errs wide."""
    if degrees < 1:
        return T95[1]
    known = [d for d in T95 if d <= degrees]
    return T95[max(known)] if degrees < 120 else 1.96


def autana_command(*words):
    return [sys.executable, str(REPO / "scripts" / "autana" / "autana.py"), *words]


def run_stamped(command, log, timeout=1800):
    """Capture wall times while enforcing a deadline on the foreground child."""
    import queue
    import threading
    started = time.monotonic()
    lines = []
    messages = queue.Queue()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, bufsize=1, cwd=REPO)
    def read():
        for line in process.stdout:
            messages.put((time.monotonic() - started, line.rstrip("\n")))
        messages.put(None)
    reader = threading.Thread(target=read)
    reader.start()
    try:
        while True:
            item = messages.get(timeout=max(0.001, timeout - (time.monotonic() - started)))
            if item is None:
                break
            at, line = item
            log.write(line + "\n")
            log.flush()
            lines.append((at, line))
        code = process.wait(timeout=max(0.001, timeout - (time.monotonic() - started)))
    except (queue.Empty, subprocess.TimeoutExpired):
        process.kill()
        process.wait()
        raise RuntimeError(f"capture timed out after {timeout}s")
    finally:
        reader.join()
        process.stdout.close()
    return code, lines, time.monotonic() - started


def captured_runs(lines, suite_name, runs):
    """The capture log of each run, in order, and each run's seconds: from its
    own `batch: ... run i/N` line to the next one, or to the end."""
    starts = [at for at, line in lines if (m := RUN_LINE.match(line)) and m.group(1) == suite_name]
    reports = [Path(m.group(1)) for _, line in lines if (m := REPORT_LINE.match(line))]
    end = lines[-1][0] if lines else 0.0
    if len(starts) != runs or len(reports) != runs:
        raise RuntimeError(f"{suite_name}: wanted {runs} runs, saw {len(starts)} starts and "
                           f"{len(reports)} reports")
    seconds = [(starts[i + 1] if i + 1 < runs else end) - starts[i] for i in range(runs)]
    return [report.with_suffix(".log") for report in reports], starts[0], seconds


def make_table(template, capture, table, project=None):
    if template == "-":
        return capture
    command = [part.replace("@CAPTURE@", str(capture)).replace("@TABLE@", str(table)).replace("@PROJECT@", str(project or REPO))
               for part in shlex.split(template)]
    command[0] = sys.executable if command[0] == "python3" else command[0]
    subprocess.run(command, check=True, cwd=REPO, capture_output=True)
    return table


def required_seeds(layout, run, runs, half_width):
    """Seeds per side so a 95% interval on B/A is within +-half_width, for
    relative layout and run spreads. None when the spread is zero."""
    per_seed = layout ** 2 + run ** 2 / runs
    if per_seed <= 0:
        return None
    def fits(seeds):
        width = t95(2 * (seeds - 1)) * math.sqrt(2 * per_seed / seeds)
        return width <= half_width
    low, high = 2, 2
    while high < 100000 and not fits(high):
        low, high = high + 1, min(100000, high * 2)
    while low < high:
        middle = (low + high) // 2
        if fits(middle):
            high = middle
        else:
            low = middle + 1
    return high



def analyse(rows_by_seed, flash_over_run, diagnostics=True):
    """The pilot numbers for one row: values per flash, each a list of runs. K is
    at the measured runs per flash, R* is what the cost ratio would favour."""
    per_seed = [values for values in rows_by_seed if len(values) >= 2]
    if len(per_seed) < 2:
        raise ValueError("a row needs at least two flashes with two runs each")
    runs = round(mean(len(values) for values in per_seed))
    grand = mean(mean(values) for values in per_seed)
    within = math.sqrt(mean(stdev(values) ** 2 for values in per_seed))
    means = [mean(values) for values in per_seed]
    seed_spread = stdev(means)
    flash_variance = max(0.0, seed_spread ** 2 - mean(stdev(values) ** 2 / len(values) for values in per_seed))
    layout = math.sqrt(flash_variance)
    optimum = (None if layout == 0 else
               max(1, math.ceil(math.sqrt(flash_over_run * within ** 2 / flash_variance))))
    relative = (layout / grand, within / grand)
    return {
        "mean": grand, "seeds": len(per_seed), "runs": runs,
        "sigma_run": relative[1], "sigma_flash": relative[0], "seed_spread": seed_spread / grand,
        "optimum_runs": optimum, "shapiro": shapiro(means) if diagnostics else None,
        "seeds_01": required_seeds(*relative, runs, 0.001),
        "seeds_05": required_seeds(*relative, runs, 0.005),
    }


def shapiro(values):
    """Shapiro-Wilk p-value of the flash means, or None without scipy or with
    fewer than three distinct values."""
    try:
        from scipy.stats import shapiro as test
    except ImportError:
        return None
    if len(set(values)) < 3:
        return None
    return float(test(values).pvalue)



def capture_metadata(capture, rows):
    text = Path(capture).read_text(encoding="utf-8", errors="replace")
    owners, instructions = {}, {}
    pending_rows, pending_insn = [], {}
    result = re.compile(r"^\S+:\d+:(\w+):(PASS|FAIL)(?::.*)?$")
    insn = re.compile(r"xtperf: scene=(\S+) event=insn .*value_per_step=(\d+)")
    for line in text.splitlines():
        timing = MEAN_RE.search(line)
        if timing:
            pending_rows.append(timing.group("name"))
        counter = insn.search(line)
        if counter and "overflow=" not in line:
            pending_insn[counter.group(1)] = int(counter.group(2))
        match = result.match(line.strip())
        if match:
            test = match.group(1)
            for row in pending_rows:
                owners[row] = test
                if row in pending_insn:
                    instructions[row] = pending_insn[row]
            if test in rows:
                owners[test] = test
                if len(pending_insn) == 1:
                    instructions[test] = next(iter(pending_insn.values()))
            pending_rows, pending_insn = [], {}
    return owners, instructions


def run_flash(args, seed, runner=None):
    runner = runner or run_stamped
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    project = Path(getattr(args, "project", REPO))
    record = {"seed": seed, "suites": {}}
    stem = f"flash_{len(list(out.glob('flash_*.json'))) + 1}"
    with open(out / f"{stem}.log", "w", encoding="utf-8") as log:
        for index, (name, tests, template) in enumerate(args.suite):
            command = autana_command("--wait", str(getattr(args, "wait", 3600)),
                                     "--project", str(project), "suite", name,
                                     str(getattr(args, "timeout", 1800)), "--runs", str(args.runs))
            if getattr(args, "autana", None):
                command = shlex.split(args.autana) + command[2:]
            if tests != "-":
                command += ["--test", tests]
            if index == 0:
                command += ["--flash", "--perf-scope", "--layout-seed", str(seed)]
            if index > 0:
                command += ["--expect-build-id", record["build_id"]]
            code, lines, wall = runner(command, log, getattr(args, "timeout", 1800) * args.runs + getattr(args, "wait", 3600))
            if code not in (0, 1):
                raise RuntimeError(f"{name} exited {code}; see {log.name}")
            captures, first_run, seconds = captured_runs(lines, name, args.runs)
            if index == 0:
                ids = [m.group(1) for _, line in lines
                       if (m := BUILD_LINE.search(line)) and "booted" in line]
                builds = list(project.glob("launcher/build*/build_id.txt"))
                expected = max(builds, key=lambda path: path.stat().st_mtime).read_text().strip() if builds else None
                if not ids or not expected or ids[-1] != expected:
                    raise RuntimeError(f"booted build {ids} does not match seeded build {expected}")
                record.update(flash_seconds=first_run, build_id=ids[-1])
            runs = []
            for number, capture in enumerate(captures):
                completion = capture.with_suffix(".md").read_text(encoding="utf-8", errors="replace")
                if "- Ended: complete" not in completion:
                    raise RuntimeError(f"incomplete capture: {capture}")
                table = make_table(template, capture, out / f"{stem}_{name}_{number + 1}.md", project)
                rows = parse_report(table)
                if not rows:
                    raise RuntimeError(f"no timing rows: {table}")
                owners, instructions = capture_metadata(capture, rows)
                runs.append(dict(capture=str(capture), table=str(table), rows=rows,
                                 owners=owners, instructions=instructions))
            record["suites"][name] = dict(run_seconds=seconds, runs=runs)
    (out / f"{stem}.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    return record
