"""Seeded capture acquisition and variance estimates shared by performance tools."""
import json
import math
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path
from statistics import mean, stdev

from seed_statistics import t_quantile

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts/lib"))
from process_tree import stop_process_tree, launch_process_tree, close_process_tree  # noqa: E402
from device_capture import RESULT_RE  # noqa: E402

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


REPORT_LINE = re.compile(r"^report: (.+\.md)\s*$")
RUN_LINE = re.compile(r"^batch: (\S+) run (\d+)/(\d+)")
BUILD_LINE = re.compile(r"BUILD_ID=(\S+)")

def autana_command(*words, override=None):
    """Use the PATH command or explicit replay command for every operation."""
    command = shlex.split(override or "autana")
    command[0] = shutil.which(command[0]) or command[0]
    return command + list(words)


def run_stamped(command, log, timeout=1800):
    """Stamp each output line; past the deadline, stop the command's whole process tree."""
    import queue
    import threading
    started = time.monotonic()
    deadline = started + timeout
    lines = []
    messages = queue.Queue()
    process = launch_process_tree(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, bufsize=1, cwd=REPO)
    def read():
        for line in process.stdout:
            messages.put((time.monotonic() - started, line.rstrip("\n")))
        messages.put(None)
    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(command, timeout)
            item = messages.get(timeout=remaining)
            if item is None:
                break
            at, line = item
            print(line, flush=True)
            log.write(line + "\n")
            log.flush()
            lines.append((at, line))
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise subprocess.TimeoutExpired(command, timeout)
        code = process.wait(timeout=remaining)
    except (queue.Empty, subprocess.TimeoutExpired):
        try:
            stop_process_tree(process)
        except subprocess.TimeoutExpired:
            pass
        raise RuntimeError(f"capture timed out after {timeout}s")
    except BaseException:
        stop_process_tree(process)
        raise
    finally:
        close_process_tree(process)
        reader.join(timeout=2)
        if not reader.is_alive():
            process.stdout.close()
    return code, lines, time.monotonic() - started


def captured_runs(lines, suite_name, runs):
    """The capture log of each run, in order, and each run's seconds: from its
    own `batch: ... run i/N` line to the next one, or to the end."""
    starts = [at for at, line in lines if (m := RUN_LINE.match(line)) and m.group(1) == suite_name]
    reports = [Path(m.group(1)) for _, line in lines if (m := REPORT_LINE.match(line))]
    end = lines[-1][0] if lines else 0.0
    refusal = capture_refusal(lines)
    if refusal:
        raise RuntimeError(refusal)
    if len(starts) != runs or len(reports) != runs:
        raise RuntimeError(f"{suite_name}: wanted {runs} runs, saw {len(starts)} starts and "
                           f"{len(reports)} reports")
    seconds = [(starts[i + 1] if i + 1 < runs else end) - starts[i] for i in range(runs)]
    return [report.with_suffix(".log") for report in reports], starts[0], seconds


def capture_refusal(lines):
    """Preserve the device verdict even when the wrapper only prints a report path."""
    refused = [line for _, line in lines if "SUITE_FILTER_REFUSED" in line]
    for _, line in lines:
        match = REPORT_LINE.match(line)
        if match:
            path = Path(match.group(1)).with_suffix(".log")
            if path.is_file():
                refused.extend(line for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
                               if "SUITE_FILTER_REFUSED" in line)
    return "\n".join(dict.fromkeys(refused))


def make_table(template, capture, table, project=None, timeout=1800):
    if template == "-":
        return capture
    command = [part.replace("@CAPTURE@", str(capture)).replace("@TABLE@", str(table)).replace("@PROJECT@", str(project or REPO))
               for part in shlex.split(template)]
    command[0] = sys.executable if command[0] == "python3" else command[0]
    with open(table.with_suffix(".command.log"), "w", encoding="utf-8") as log:
        code, lines, _ = run_stamped(command, log, timeout)
    if code:
        raise RuntimeError(f"table command {command}: " + "\n".join(line for _, line in lines))
    return table


def required_seeds(layout, run, runs, half_width, alpha=0.025):
    """Seeds per side for a one-sided alpha interval within the relative margin."""
    per_seed = layout ** 2 + run ** 2 / runs
    if per_seed <= 0:
        return None
    def fits(seeds):
        width = t_quantile(1 - alpha, 2 * (seeds - 1)) * math.sqrt(2 * per_seed / seeds)
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
    grand = mean(mean(values) for values in rows_by_seed)
    within = math.sqrt(mean(stdev(values) ** 2 for values in per_seed))
    means = [mean(values) for values in rows_by_seed]
    seed_spread = stdev(means)
    flash_variance = max(0.0, seed_spread ** 2 - mean((stdev(values) ** 2 if len(values) >= 2 else within ** 2) / len(values)
                                  for values in rows_by_seed))
    layout = math.sqrt(flash_variance)
    optimum = (None if layout == 0 else
               max(1, math.ceil(math.sqrt(flash_over_run * within ** 2 / flash_variance))))
    relative = (layout / grand, within / grand)
    return {
        "mean": grand, "seeds": len(rows_by_seed), "runs": runs,
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
    insn = re.compile(r"xtperf: scene=(\S+) event=insn .*value_per_step=(\d+)")
    for line in text.splitlines():
        timing = MEAN_RE.search(line)
        if timing:
            pending_rows.append(timing.group("name"))
        counter = insn.search(line)
        if counter and "overflow=" not in line:
            pending_insn[counter.group(1)] = int(counter.group(2))
        match = RESULT_RE.match(line.strip())
        if match:
            test = match.group("name")
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
    """Capture repeated suites on one seeded image with status and identity checks."""
    runner = runner or run_stamped
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    project = Path(getattr(args, "project", REPO))
    record = {"seed": seed, "suites": {}}
    override = getattr(args, "autana", None)
    attempts = [int(match.group(1)) for path in out.iterdir()
                if (match := re.match(r"flash_(\d+)(?:[_.])", path.name))]
    stem = f"flash_{max(attempts, default=0) + 1}"
    def status(phase):
        with open(out / f"{stem}_status_{phase}.log", "w", encoding="utf-8") as status_log:
            code, lines, _ = runner(autana_command("status", override=override), status_log, 30)
            if not status_log.tell():
                status_log.write("\n".join(line for _, line in lines) + "\n")
            status_log.write(f"exit code: {code}\n")
            if code and phase == "before":
                raise RuntimeError(f"autana status before exited {code}")

    validate_filters(args.suite, filter_limits(project))
    try:
        status("before")
        with open(out / f"{stem}.log", "w", encoding="utf-8") as log:
            for index, (name, tests, template) in enumerate(args.suite):
                command = autana_command("--wait", str(getattr(args, "wait", 3600)),
                                         "--project", str(project), "suite", name,
                                         str(getattr(args, "timeout", 1800)), "--runs", str(args.runs),
                                         override=override)
                if tests != "-":
                    command += ["--test", tests]
                if index == 0:
                    command += ["--flash", "--perf-scope", "--layout-seed", str(seed)]
                if index > 0:
                    command += ["--expect-build-id", record["build_id"]]
                code, lines, wall = runner(command, log, getattr(args, "timeout", 1800) * args.runs + getattr(args, "wait", 3600))
                if code not in (0, 1):
                    refusal = capture_refusal(lines)
                    raise RuntimeError(refusal or f"{name} exited {code}; see {log.name}")
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
                    table = make_table(template, capture, out / f"{stem}_{name}_{index}_{number + 1}.md", project, getattr(args, "timeout", 1800))
                    rows = parse_report(table)
                    owners, instructions = capture_metadata(capture, rows)
                    runs.append(dict(capture=str(capture), table=str(table), rows=rows,
                                     owners=owners, instructions=instructions, listed_tests=capture_tests(capture), inventory=capture_tests(capture) if tests == "-" else []))
                if name not in record["suites"]:
                    record["suites"][name] = dict(tests=tests, run_seconds=seconds, runs=runs)
                else:
                    entry = record["suites"][name]
                    entry["tests"] += "," + tests
                    for number, run in enumerate(runs):
                        entry["run_seconds"][number] += seconds[number]
                        previous = entry["runs"][number]
                        for key in ("rows", "owners", "instructions"):
                            previous[key].update(run[key])
                        previous["listed_tests"].extend(run["listed_tests"])
                        previous["inventory"].extend(run["inventory"])
                        previous.setdefault("tables", [previous["table"]]).append(run["table"])
                        previous.setdefault("captures", [previous["capture"]]).append(run["capture"])
        (out / f"{stem}.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        return record
    finally:
        try:
            status("after")
        except Exception as error:
            with open(out / f"{stem}_status_after.log", "a", encoding="utf-8") as log:
                log.write(f"status after error: {error}\n")


def filter_limits(project):
    """Read the compared firmware's accepted pattern width and count."""
    text = (Path(project) / "launcher/test/suites.h").read_text(encoding="utf-8")
    def define(name):
        match = re.search(r"^\s*#define\s+" + name + r"\s+(\d+)\b", text, re.MULTILINE)
        if not match or int(match.group(1)) <= 0:
            raise ValueError(f"invalid filter limit {name}")
        return int(match.group(1))
    return define("SUITE_FILTER_LEN") - 1, define("SUITE_FILTER_MAX")


def validate_filters(suites, limits):
    """Refuse user filters before any seeded flash."""
    width, count = limits
    for _, tests, _ in suites:
        if tests != "-" and (len(tests.split(",")) > count or
                             any(not test or len(test) > width for test in tests.split(","))):
            raise ValueError(f"suite filter exceeds project limits: {width} characters, {count} patterns")


def capture_tests(capture):
    """List all tests reported by the capture, including tests without timings."""
    text = Path(capture).read_text(encoding="utf-8", errors="replace")
    return [match.group("name") for line in text.splitlines()
            if (match := RESULT_RE.match(line.strip()))]
