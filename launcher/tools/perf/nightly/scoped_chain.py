"""Plan and acquire board-local first-parent performance chains."""
import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timedelta
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
from types import SimpleNamespace

PERF = Path(__file__).resolve().parents[1]
REPO = PERF.parents[2]
sys.path.insert(0, str(PERF))
sys.path.insert(0, str(PERF.parent / "build"))
from build_paths import remove_output
import autana_config
from capture import require_same_board
from device_lock import normalise_board, short_id, write_json
from layout_measure import autana_command, run_flash, run_stamped, REPORT_LINE, latest_build
from process_tree import launch_process_tree, stop_process_tree, close_process_tree
from local_schedule import (Clock, wait_idle, gpu_pending, gpu_on_host, GPU_NOT_ON_HOST,
                            NIGHT_STEP_HANG_SECONDS, MARKER_KEY_FORMAT, heavy_root)
from nightly.lnt import lnt_report, night_manifest, read_last_night, VARIANT, BUILD_FLAGS, LAYOUT_SEED

IMAGE_HEADER_BYTES = 24
SEGMENT_HEADER_BYTES = 8
APP_DESC_OFFSET = IMAGE_HEADER_BYTES + SEGMENT_HEADER_BYTES
APP_DESC_BYTES = 256
IMAGE_SEGMENT_COUNT_OFFSET = 1
SEGMENT_DATA_LENGTH_OFFSET = 4
SEGMENT_DATA_LENGTH_BYTES = 4
IMAGE_HASH_DIGITS = 16
REPORT_SHA_DIGITS = 12
STEP_INDEX_DIGITS = 2
FIRST_STEP_SECONDS = 23 * 60
RESCAN_SECONDS = 60
CAPTURE_FAILURE_LIMIT = 2
SELFTEST_ALLOWANCE_SECONDS = 120 * 60
CAPTURE_TIMEOUT_SECONDS = 1800
BUILD_TIMEOUT_SECONDS = 3600
LANE_HANG_FACTOR = 2
# A command that reaches the boards of other machines; unset, the night uses this machine's only.
# `<command> boards` prints {"boards": [{"board", "port", "host"}, ...]}; `<command> run HOST BOARD
# FOLDER -- SCRIPT ARGS...` runs this checkout's SCRIPT on HOST's BOARD under one device lock, with
# FOLDER copied there first and back after, and paths in ARGS naming their copies there.
REMOTE_LANE_ENV = "REMOTE_LANE_COMMAND"


@dataclass
class Step:
    commit: str
    pr: int | None
    role: str
    position: int


@dataclass
class StepResult:
    step: Step
    outcome: str
    image_hash: str | None = None
    build_id: str | None = None
    wait_seconds: float = 0
    build_seconds: float = 0
    measure_seconds: float = 0
    report: str | None = None


def image_hash(build_dir):
    directory = Path(build_dir)
    app = (directory / "launcher.bin").read_bytes()
    if len(app) < APP_DESC_OFFSET + APP_DESC_BYTES:
        raise ValueError("application image is too short")
    end = IMAGE_HEADER_BYTES
    for _ in range(app[IMAGE_SEGMENT_COUNT_OFFSET]):
        length_at = end + SEGMENT_DATA_LENGTH_OFFSET
        length = int.from_bytes(app[length_at:length_at + SEGMENT_DATA_LENGTH_BYTES], "little")
        end += SEGMENT_HEADER_BYTES + length
        if end > len(app):
            raise ValueError("application segment exceeds image")
    if end < APP_DESC_OFFSET + APP_DESC_BYTES:
        raise ValueError("application segments omit app descriptor")
    assets = (directory / "assets.bin").read_bytes()
    image = app[:APP_DESC_OFFSET] + bytes(APP_DESC_BYTES) + app[APP_DESC_OFFSET + APP_DESC_BYTES:end]
    return hashlib.sha256(image + assets).hexdigest()[:IMAGE_HASH_DIGITS]


def git(repo, *words):
    return subprocess.check_output(["git", "-C", str(repo), "-c", "core.longpaths=true", *words], text=True, stderr=subprocess.STDOUT).strip()


def history_steps(repo, tip):
    history = git(repo, "log", "--first-parent", "--reverse", "--format=%H%x00%s", tip)
    steps = []
    for position, line in enumerate(history.splitlines(), start=1):
        commit, subject = line.split("\0", 1)
        matches = list(re.finditer(r"\(#(\d+)\)|Merge pull request #(\d+)", subject))
        pr = matches[-1] if matches else None
        steps.append(Step(commit, int(pr[1] or pr[2]) if pr else None, "new", position))
    return steps


def plan_steps(repo, tip, last_night, lane, lanes):
    if lanes <= 0 or not 0 <= lane < lanes:
        raise ValueError("lane must be in [0, lanes)")
    history = history_steps(repo, tip)
    commits = [step.commit for step in history]
    anchor = (last_night or {}).get("last_measured")
    if anchor and anchor not in commits:
        raise ValueError("last_measured is not on the tip's first-parent history")
    attempted = (last_night or {}).get("last_attempted") or anchor
    if attempted and attempted not in commits:
        raise ValueError("last_attempted is not on the tip first-parent history")
    attempted_position = commits.index(attempted) + 1 if attempted else 0
    selected = []
    for step in history:
        if step.commit == anchor:
            step.role = "anchor"
            selected.append(step)
        elif step.position > attempted_position and step.position % lanes == lane:
            selected.append(step)
    return selected if anchor else selected[-1:]


def select_steps(repo, tip, previous, lane, lanes, from_sha=None, max_steps=None):
    steps = plan_steps(repo, tip, previous, lane, lanes)
    if from_sha:
        first = git(repo, "rev-parse", from_sha + "^{commit}")
        history = history_steps(repo, tip)
        commits = [step.commit for step in history]
        if first not in commits:
            raise ValueError("--from is not on the tip's first-parent history")
        # --from replaces the planned new steps; the anchor stays.
        steps = [step for step in steps if step.role == "anchor"]
        steps.extend(step for step in history[commits.index(first):]
                     if step.position % lanes == lane and step.commit != (previous or {}).get("last_measured"))
    if max_steps is not None:
        if max_steps < 0:
            raise ValueError("--max-steps must be nonnegative")
        anchors = [step for step in steps if step.role == "anchor"]
        steps = anchors + [step for step in steps if step.role == "new"][:max_steps]
    return steps


def run_chain(steps, args, runner=None, clock=None):
    runner, clock = runner or run_stamped, clock or Clock()
    started = clock.now()
    night_started = datetime.fromisoformat(args.started) if getattr(args, "started", None) else started
    night = getattr(args, "night", None) or started.date().isoformat()
    output = Path(args.out).resolve()
    output.mkdir(parents=True, exist_ok=True)
    board = short_id(args.board)
    previous = getattr(args, "previous", None) or {}
    machine = previous.get("machine")
    last_measured = previous.get("last_measured")
    last_attempted = previous.get("last_attempted") or last_measured
    previous_hash = previous.get("last_image_hash")
    positions = {step.commit: step.position for step in history_steps(args.repo, "HEAD")}
    results, failures, stop_reason = [], 0, None
    ended = None
    selftest = dict(board=board, outcome="not_run", report=None, started=None, seconds=0)
    manifest = Path(args.data) / "scoped" / board / night_started.strftime(MARKER_KEY_FORMAT) / "night.json"

    cleanup = "pending"

    def save():
        value = night_manifest(board, machine, night, args.lane, results, stop_reason, last_measured, selftest,
                               started.isoformat(), ended)
        value["last_attempted"] = last_attempted
        value["cleanup"] = cleanup
        value["last_image_hash"] = previous_hash
        write_json(manifest, value)

    def gone():
        try:
            return board not in plugged_boards()
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            print(f"devices: {error}", file=sys.stderr)
            return False

    def run_selftest():
        if not getattr(args, "selftest", None):
            return
        begin = clock.now()
        selftest["started"] = begin.isoformat()
        log_path = output / "selftest.log"
        selftest["report"] = str(log_path.relative_to(output))
        with log_path.open("w", encoding="utf-8") as log:
            try:
                code, lines, _ = runner(args.selftest, log, SELFTEST_ALLOWANCE_SECONDS)
                selftest["outcome"] = "failed" if code else "passed"
                reports = [match[1] for _, line in lines if (match := REPORT_LINE.match(line))]
                selftest["report"] = reports[-1] if reports else str(log_path.relative_to(output))
            except (OSError, RuntimeError) as error:
                log.write(str(error) + "\n")
                selftest["outcome"] = "failed"
        selftest["seconds"] = (clock.now() - begin).total_seconds()
        save()

    save()
    directory = tempfile.mkdtemp(prefix="nightly-", dir=output)
    worktree = Path(directory) / "tree"
    try:
        run_selftest()
        for index, step in enumerate(steps):
            if not stop_reason and gone():
                stop_reason = "unplugged"
            outcome = {"capture_failures": "not_run_stopped", "unplugged": "not_run_unplugged", "busy": "not_run_busy"}
            result = StepResult(step, outcome.get(stop_reason, "not_run"))
            results.append(result)
            if stop_reason:
                save()
                continue
            def run_step(command, log, timeout, **options):
                response = runner(command, log, timeout, **options)
                result.wait_seconds += sum(float(line.removeprefix(autana_config.HEAVY_WAIT_PREFIX))
                                           for _, line in response[1] if line.startswith(autana_config.HEAVY_WAIT_PREFIX))
                return response

            begin = clock.now()
            result.outcome = "build_failed"
            code = None
            with (output / f"{index:0{STEP_INDEX_DIGITS}}-{step.commit[:REPORT_SHA_DIGITS]}-build.log").open("w", encoding="utf-8") as log:
                try:
                    if not worktree.exists():
                        git(args.repo, "worktree", "add", "--detach", str(worktree), step.commit)
                        # Captures land with the lane's output, on whichever machine runs it.
                        (worktree / autana_config.CONFIG_NAME).write_text(
                            f"records = '{(output / 'device').as_posix()}'\n", encoding="utf-8")
                    git(worktree, "checkout", "--detach", step.commit)
                    git(worktree, "submodule", "update", "--init", "--recursive")
                    code, _, _ = run_step(autana_command("--project", str(worktree), "build", VARIANT,
                                                      *BUILD_FLAGS), log, BUILD_TIMEOUT_SECONDS)
                    if code == autana_config.EXIT_BUSY:
                        stop_reason = "busy"
                        result.outcome = "not_run_busy"
                    if code:
                        raise RuntimeError(f"build exited {code}")
                    build = latest_build(worktree)
                    result.build_id = (build / "build_id.txt").read_text().strip()
                    result.image_hash = image_hash(build)
                except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
                    log.write(str(error) + "\n")
            result.build_seconds = max(0, (clock.now() - begin).total_seconds() - result.wait_seconds)
            if result.image_hash:
                if step.role != "anchor" and result.image_hash == previous_hash:
                    result.outcome = "skipped_same_image"
                else:
                    measure = clock.now()
                    result.outcome = "capture_failed"
                    try:
                        capture_args = SimpleNamespace(project=worktree, out=output / f"{index:0{STEP_INDEX_DIGITS}}-capture",
                                                       suite=args.suite, runs=1, timeout=CAPTURE_TIMEOUT_SECONDS,
                                                       wait=NIGHT_STEP_HANG_SECONDS)
                        record = run_flash(capture_args, seed=LAYOUT_SEED, runner=run_step)
                        actual = require_same_board({"previous": machine, "capture": record["board"]}
                                                    if machine else {"capture": record["board"]})
                        if not actual or normalise_board(args.board) not in (normalise_board(actual), short_id(actual)):
                            raise ValueError("capture does not identify the requested board")
                        if record["build_id"] != result.build_id:
                            raise ValueError(f"flashed build {record['build_id']} differs from hashed build {result.build_id}")
                        machine = actual
                        rows = {}
                        for suite, entry in record["suites"].items():
                            for run in entry["runs"]:
                                for row, value in run["rows"].items():
                                    rows.setdefault(suite + "/" + row, []).append(value)
                        if not rows:
                            raise ValueError("capture has no timing rows")
                        result.report = f"{index:0{STEP_INDEX_DIGITS}}-{step.commit[:REPORT_SHA_DIGITS]}.json"
                        write_json(manifest.parent / result.report,
                                   lnt_report(machine, board, step.commit, step.pr, step.role, night,
                                              record["build_id"], result.image_hash, measure.isoformat(),
                                              clock.now().isoformat(), rows))
                        result.outcome = "measured"
                        if step.position >= positions.get(last_measured, 0):
                            previous_hash, last_measured = result.image_hash, step.commit
                        failures = 0
                    except (OSError, RuntimeError, ValueError) as error:
                        (output / f"{index:0{STEP_INDEX_DIGITS}}-error.log").write_text(str(error), encoding="utf-8")
                        if gone():
                            stop_reason = "unplugged"
                            result.outcome = "not_run_unplugged"
                        else:
                            failures += 1
                            if failures >= CAPTURE_FAILURE_LIMIT:
                                stop_reason = "capture_failures"
                    result.measure_seconds = (clock.now() - measure).total_seconds()
            if result.outcome in ("measured", "skipped_same_image", "build_failed"):
                if step.position >= positions.get(last_attempted, 0):
                    last_attempted = step.commit
            save()
    finally:
        errors = []
        try:
            records = worktree / ".records/device"
            if records.exists():
                shutil.copytree(records, output / "device", dirs_exist_ok=True)
        except OSError as error:
            errors.append(str(error))
        try:
            if worktree.exists():
                git(args.repo, "worktree", "remove", "--force", "--force", str(worktree))
        except (OSError, subprocess.CalledProcessError) as error:
            errors.append(str(error) + ": " + str(getattr(error, "output", None) or getattr(error, "stderr", "")))
            try:
                remove_output(worktree)
            except OSError as error:
                errors.append(str(error))
        try:
            remove_output(directory)
        except OSError as error:
            errors.append(str(error))
        try:
            if steps:
                git(args.repo, "worktree", "prune")
        except (OSError, subprocess.CalledProcessError) as error:
            errors.append(str(error))
        cleanup = "failed: " + "; ".join(errors) if errors else "passed"
        try:
            if errors:
                (output / "cleanup.log").write_text(cleanup + "\n", encoding="utf-8")
        except OSError as error:
            cleanup += "; cleanup log: " + str(error)
        finally:
            ended = clock.now().isoformat()
            save()
    return results


def plugged_boards(autana=None):
    """Short ids of the boards on this machine's USB, in `autana devices` order."""
    listing = subprocess.check_output(autana_command("devices", "--json", override=autana), text=True)
    return [short_id(board["board"]) for board in json.loads(listing)["boards"] if board["port"]]


def remote_command():
    return shlex.split(os.environ.get(REMOTE_LANE_ENV, ""))


def remote_boards():
    """The other machines' plugged boards from the remote-lane command: short id, port and host."""
    if not remote_command():
        return []
    try:
        listing = subprocess.check_output([*remote_command(), "boards"], text=True)
        return [dict(board=short_id(board["board"]), port=board["port"], host=board["host"])
                for board in json.loads(listing)["boards"] if board["port"]]
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        # An unreachable host costs its lanes, not the night.
        print(f"remote boards: {error}", file=sys.stderr)
        return []


def board_listing():
    """Every board the night can reach: this machine's `autana devices` entries, then the remote ones."""
    listing = json.loads(subprocess.check_output(autana_command("devices", "--json"), text=True))
    local = [board for board in listing["boards"] if board["port"]]
    seen = {short_id(board["board"]) for board in local}
    return dict(boards=local + [board for board in remote_boards() if board["board"] not in seen])


def plan_lane(args, selection, board, lane, lanes, selftest=False, host=None):
    previous = read_last_night(args.data, board)
    steps = select_steps(args.repo, selection["tip"], previous, lane, lanes,
                         selection.get("from_sha"), selection.get("max_steps"))
    planned = dict(lane=f"{lane}/{lanes}", previous=previous, steps=[asdict(step) for step in steps],
                   estimate=len(steps) * FIRST_STEP_SECONDS + (SELFTEST_ALLOWANCE_SECONDS if selftest else 0))
    return dict(planned, host=host) if host else planned


def run_night(args, clock=None, launch=None):
    clock, launch = clock or Clock(), launch or launch_process_tree
    planned = json.loads(args.plan.read_text(encoding="utf-8"))
    processes, skipped = [], set()
    failed, interrupted = False, False
    hung = set()

    def start(board, lane, lane_plan):
        if lane.get("host"):
            return start_remote(board, lane, lane_plan)
        wait = NIGHT_STEP_HANG_SECONDS
        command = autana_command("--board", board, "--wait", str(wait), "lock", "run",
                                 "--estimate", str(lane["estimate"]), "--", sys.executable,
                                 str(Path(__file__).resolve()), "run", "--plan", str(lane_plan.resolve()),
                                 "--repo", str(args.repo.resolve()), "--data", str(args.data.resolve()),
                                 "--out", str(args.out.resolve() / board), "--board", board, "--lane", lane["lane"])
        for suite in args.suite:
            command.extend(["--suite", *suite])
        env = dict(os.environ)
        env.pop("BOARD_CI_LOG", None)
        if board == env.get("SELFTEST_BOARD"):
            shell = shutil.which("bash")
            if not shell:
                raise OSError("bash is required for the self-test")
            env["BOARD_CI_LOG"] = str(args.out.resolve() / f"{board}-selftest.log")
            if cygpath := shutil.which("cygpath"):
                shell = subprocess.check_output([cygpath, "-w", shell], text=True).strip()
                env["BOARD_CI_LOG"] = subprocess.check_output(
                    [cygpath, "-u", env["BOARD_CI_LOG"]], text=True).strip()
            command.extend(["--selftest", shell, str(args.repo.resolve() / "scripts/ci/board-steps.sh")])
        process = launch(command, cwd=args.repo, env=env)
        processes.append((process, clock.now(), lane["estimate"], board))

    def start_remote(board, lane, lane_plan):
        # The lane's folder travels: its plan in, its data and captures back.
        folder = args.out.resolve() / board
        write_json(folder / "lane.json", dict(json.loads(lane_plan.read_text(encoding="utf-8")), lanes={board: lane}))
        script = args.repo.resolve() / Path(__file__).resolve().relative_to(REPO)
        command = [*remote_command(), "run", lane["host"], board, str(folder), "--", str(script), "run",
                   "--plan", str(folder / "lane.json"), "--repo", str(args.repo.resolve()),
                   "--data", str(folder / "data"), "--out", str(folder), "--board", board, "--lane", lane["lane"]]
        for suite in args.suite:
            command.extend(["--suite", *suite])
        env = dict(os.environ)
        env.pop("BOARD_CI_LOG", None)
        process = launch(command, cwd=args.repo, env=env)
        processes.append((process, clock.now(), lane["estimate"], board))

    previous_handler = signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        for board in planned["lanes"]:
            try:
                start(board, planned["lanes"][board], args.plan)
            except (OSError, ValueError, subprocess.CalledProcessError) as error:
                print(f"lane {board}: {error}", file=sys.stderr)
                failed = True
                skipped.add(board)
        while any(id(process) not in hung and process.poll() is None for process, *_ in processes):
            clock.sleep(RESCAN_SECONDS)
            for process, started, estimate, board in processes:
                if id(process) not in hung and process.poll() is None and (clock.now() - started).total_seconds() >= LANE_HANG_FACTOR * max(estimate, FIRST_STEP_SECONDS):
                    print(f"LANE_HANG_FACTOR expired for board {board}", file=sys.stderr)
                    stop_process_tree(process)
                    hung.add(id(process))
                    failed = True
            try:
                boards = plugged_boards()
            except (OSError, ValueError, subprocess.CalledProcessError) as error:
                print(f"devices: {error}", file=sys.stderr)
                failed, boards = True, []
            for board in boards:
                if board in planned["lanes"] or board in skipped:
                    continue
                try:
                    lane = plan_lane(args, planned, board, 0, 1)
                    lane_plan = args.out / board / "lane.json"
                    write_json(lane_plan, dict(planned, lanes={board: lane}))
                    start(board, lane, lane_plan)
                    planned["lanes"][board] = lane
                    write_json(args.plan, planned)
                except (OSError, ValueError, subprocess.CalledProcessError) as error:
                    print(f"lane {board}: {error}", file=sys.stderr)
                    failed = True
                    skipped.add(board)
    except KeyboardInterrupt:
        interrupted = True
    finally:
        try:
            pending = [process for process, *_ in processes]
            while pending:
                try:
                    if interrupted:
                        for process in pending:
                            if process.poll() is None:
                                stop_process_tree(process)
                    if pending[0].wait():
                        failed = True
                    close_process_tree(pending.pop(0))
                except KeyboardInterrupt:
                    interrupted = True
            for board, lane in planned["lanes"].items():
                returned = args.out.resolve() / board / "data" / "scoped"
                if lane.get("host") and returned.is_dir():
                    shutil.copytree(returned, args.data / "scoped", dirs_exist_ok=True)
        finally:
            signal.signal(signal.SIGTERM, previous_handler)
    if interrupted:
        raise KeyboardInterrupt
    return int(failed)


def plan(args, clock=None, request=None):
    clock = clock or Clock()
    marker = datetime.fromisoformat(args.marker)
    if gpu_on_host(clock):
        wait_idle(lambda: gpu_pending(request=request), clock, idle_polls=2)
    else:
        print(GPU_NOT_ON_HOST, file=sys.stderr)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
                summary.write(GPU_NOT_ON_HOST + "\n")
    now = clock.now()
    remote = {board["board"]: board["host"] for board in remote_boards()}
    if not args.boards:
        local = plugged_boards()
        args.boards = local + [board for board in remote if board not in local]
        if not args.boards:
            raise SystemExit("plan: no board is plugged in")
        # The self-test runs on one board of this machine, whichever is plugged in first.
        args.selftest_board = args.selftest_board or next(iter(local), None)
        remote = {board: host for board, host in remote.items() if board not in local}
    tip = git(args.repo, "rev-parse", args.tip + "^{commit}")
    planned = dict(tip=tip, night=marker.date().isoformat(), started=now.isoformat(),
                   marker=args.marker, lanes={}, from_sha=args.from_sha, max_steps=args.max_steps)
    lanes = planned["lanes"]
    for lane, board in enumerate(args.boards):
        board = short_id(board)
        lanes[board] = plan_lane(args, planned, board, lane, len(args.boards),
                                 args.selftest_board and board == short_id(args.selftest_board), remote.get(board))
    write_json(Path(args.out), planned)
    # For the workflow's later steps, which run one lane per board.
    print("SCOPED_BOARDS=" + " ".join(lanes))
    print("SELFTEST_BOARD=" + (short_id(args.selftest_board) if args.selftest_board else ""))


def timeline(args):
    planned = json.loads(args.plan.read_text(encoding="utf-8"))
    marker = datetime.fromisoformat(planned["marker"])
    key = marker.strftime(MARKER_KEY_FORMAT)
    steps = []
    for path in sorted((heavy_root() / "night-steps" / key).glob("*.json")):
        record = json.loads(path.read_text(encoding="utf-8"))
        steps.append(dict(name="gpu " + record["workflow"], started=record["started"],
                          ended=record["ended"], outcome="failed" if record["exit"] else "passed"))
    lane_key = datetime.fromisoformat(planned["started"]).strftime(MARKER_KEY_FORMAT)
    for board in planned["lanes"]:
        path = args.data / "scoped" / board / lane_key / "night.json"
        if not path.is_file():
            continue
        record = json.loads(path.read_text(encoding="utf-8"))
        failed = record["selftest"]["outcome"] == "failed" or any(
            step["outcome"] in ("build_failed", "capture_failed") for step in record["steps"])
        steps.append(dict(name="board " + board, started=record["started"], ended=record["ended"],
                          outcome="failed" if failed else "passed" if record["ended"] else "running"))
    ended = [datetime.fromisoformat(step["ended"]) for step in steps if step["ended"]]
    free_at = max(ended, default=marker)
    write_json(args.data / "scoped" / "timeline" / (key + ".json"),
               dict(marker=planned["marker"], steps=steps, free_at=free_at.isoformat()))
    if args.summary:
        spans = [step["name"] + " " + datetime.fromisoformat(step["started"]).astimezone().strftime("%H:%M")
                 + "–" + (datetime.fromisoformat(step["ended"]).astimezone().strftime("%H:%M")
                          if step["ended"] else "running") for step in steps]
        hours, remainder = divmod(free_at - marker, timedelta(hours=1))
        minutes = remainder // timedelta(minutes=1)
        line = (f"Night {marker:%Y-%m-%d}: marker {marker:%H:%M}, " + ", ".join(spans)
                + f", host free {free_at.astimezone():%H:%M} ({hours}h {minutes:02d}min after the marker)\n")
        with args.summary.open("a", encoding="utf-8") as summary:
            summary.write(line)


def lane_of(plan, board):
    return plan["lanes"][short_id(board)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("boards", help="every board the night can reach, as `autana devices --json` lists them")
    timing = commands.add_parser("timeline")
    timing.add_argument("--plan", type=Path, required=True)
    timing.add_argument("--data", type=Path, required=True)
    timing.add_argument("--summary", type=Path)
    planning = commands.add_parser("plan")
    planning.add_argument("--boards", nargs="+", help="default: every board `autana devices` lists as plugged in")
    planning.add_argument("--tip", default="origin/main")
    planning.add_argument("--marker", required=True)
    planning.add_argument("--selftest-board")
    night = commands.add_parser("night")
    night.add_argument("--plan", type=Path, required=True)
    night.add_argument("--suite", nargs=3, action="append", default=[])
    running = commands.add_parser("run")
    running.add_argument("--plan", type=Path, required=True)
    running.add_argument("--board", required=True)
    running.add_argument("--lane", required=True)
    running.add_argument("--suite", nargs=3, action="append", default=[])
    running.add_argument("--selftest", nargs=argparse.REMAINDER)
    for command in (planning, running, night):
        command.add_argument("--repo", type=Path, default=REPO)
        command.add_argument("--data", type=Path, required=True)
        command.add_argument("--out", type=Path, required=True)
    for command in (planning, running):
        command.add_argument("--from", dest="from_sha")
        command.add_argument("--max-steps", type=int)
    args = parser.parse_args()
    os.environ[autana_config.BUILD_PRIORITY_ENV] = autana_config.BUILD_PRIORITY_BELOW_NORMAL
    if args.command == "boards":
        print(json.dumps(board_listing()))
        return 0
    if args.command == "timeline":
        timeline(args)
        return 0
    if args.command == "plan":
        plan(args)
        return 0
    if not args.suite:
        parser.error(f"{args.command} requires at least one --suite")
    if args.command == "night":
        return run_night(args)
    if not os.environ.get("_AUTANA_DEVICE_LOCK_TOKEN"):
        parser.error("run requires an enclosing autana lock run hold")
    planned = json.loads(args.plan.read_text(encoding="utf-8"))
    lane = lane_of(planned, args.board)
    if lane["lane"] != args.lane:
        parser.error("lane differs from the shared plan")
    args.previous = lane["previous"]
    args.night, args.started = (planned[key] for key in ("night", "started"))
    steps = [Step(**step) for step in lane["steps"]]
    if args.from_sha is not None or args.max_steps is not None:
        index, count = map(int, args.lane.split("/"))
        steps = select_steps(args.repo, planned["tip"], args.previous, index, count, args.from_sha, args.max_steps)
    results = run_chain(steps, args)
    manifest = read_last_night(args.data, short_id(args.board))
    return int(any(result.outcome in ("build_failed", "capture_failed") for result in results)
               or manifest["selftest"]["outcome"] == "failed")


if __name__ == "__main__":
    sys.exit(main())
