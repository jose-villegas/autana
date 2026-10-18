"""Plan and acquire board-local first-parent performance chains."""
import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timedelta
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

PERF = Path(__file__).resolve().parents[1]
REPO = PERF.parents[2]
sys.path.insert(0, str(PERF))
sys.path.insert(0, str(PERF.parent / "build"))
from build_paths import remove_output
from capture import require_same_board
from device_lock import normalise_board, short_id, write_json
from layout_measure import autana_command, run_flash, run_stamped, REPORT_LINE, latest_build
from nightly.lnt import lnt_report, night_manifest, read_last_night

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
CAPTURE_FAILURE_LIMIT = 2
SELFTEST_ALLOWANCE_SECONDS = 120 * 60
CAPTURE_TIMEOUT_SECONDS = 1800
BUILD_TIMEOUT_SECONDS = 3600


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


class Clock:
    def now(self):
        return datetime.now().astimezone()

    def sleep(self, seconds):
        time.sleep(seconds)


def local_time(value, now, next_occurrence=False):
    hour, minute = map(int, value.split(":"))
    target = now.replace(hour=hour, minute=minute, second=0, microsecond=0)
    if next_occurrence and target <= now:
        target += timedelta(days=1)
    return target


def run_chain(steps, args, runner=None, clock=None):
    runner, clock = runner or run_stamped, clock or Clock()
    started = clock.now()
    night_started = datetime.fromisoformat(args.started) if getattr(args, "started", None) else started
    deadline = local_time(args.until, night_started, next_occurrence=True)
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
    results, failures, worst_step, stop_reason = [], 0, FIRST_STEP_SECONDS, None
    selftest = dict(board=board, outcome="not_run", report=None, started=None, seconds=0)
    selftest_at = (local_time(args.selftest_at, night_started, next_occurrence=True)
                   if getattr(args, "selftest_at", None) else None)
    manifest = Path(args.data) / "scoped" / board / night_started.strftime("%Y-%m-%d-%H%M") / "night.json"

    cleanup = "pending"

    def save():
        value = night_manifest(board, machine, night, args.lane, results, stop_reason, last_measured, selftest)
        value["last_attempted"] = last_attempted
        value["cleanup"] = cleanup
        value["last_image_hash"] = previous_hash
        write_json(manifest, value)

    def test_if_due(wait=False):
        if not selftest_at or selftest["outcome"] != "not_run":
            return
        if wait and clock.now() < selftest_at:
            if selftest_at + timedelta(seconds=SELFTEST_ALLOWANCE_SECONDS) > deadline:
                return
            clock.sleep((selftest_at - clock.now()).total_seconds())
        if clock.now() < selftest_at or clock.now() + timedelta(seconds=SELFTEST_ALLOWANCE_SECONDS) > deadline:
            return
        begin = clock.now()
        selftest["started"] = begin.isoformat()
        with (output / "selftest.log").open("w", encoding="utf-8") as log:
            try:
                code, lines, _ = runner(args.selftest, log, SELFTEST_ALLOWANCE_SECONDS)
                selftest["outcome"] = "failed" if code else "passed"
                reports = [match[1] for _, line in lines if (match := REPORT_LINE.match(line))]
                selftest["report"] = reports[-1] if reports else None
            except (OSError, RuntimeError) as error:
                log.write(str(error) + "\n")
                selftest["outcome"] = "failed"
        selftest["seconds"] = (clock.now() - begin).total_seconds()
        save()

    save()
    directory = tempfile.mkdtemp(prefix="nightly-", dir=output)
    worktree = Path(directory) / "tree"
    try:
        if steps:
            git(args.repo, "worktree", "add", "--detach", str(worktree), steps[0].commit)
        for index, step in enumerate(steps):
            test_if_due()
            reserve = (SELFTEST_ALLOWANCE_SECONDS if selftest_at and selftest["outcome"] == "not_run" else 0)
            if not stop_reason and clock.now() + timedelta(seconds=worst_step + reserve) > deadline:
                stop_reason = "window"
            result = StepResult(step, "not_run_stopped" if stop_reason == "capture_failures" else "not_run_window")
            results.append(result)
            if stop_reason:
                save()
                continue
            begin = clock.now()
            result.outcome = "build_failed"
            with (output / f"{index:0{STEP_INDEX_DIGITS}}-{step.commit[:REPORT_SHA_DIGITS]}-build.log").open("w", encoding="utf-8") as log:
                try:
                    git(worktree, "checkout", "--detach", step.commit)
                    git(worktree, "submodule", "update", "--init", "--recursive")
                    code, _, _ = runner(autana_command("--project", str(worktree), "build", "diag",
                                                      "--perf-scope", "--layout-seed", "0"), log,
                                         BUILD_TIMEOUT_SECONDS)
                    result.build_seconds = (clock.now() - begin).total_seconds()
                    if code:
                        raise RuntimeError(f"build exited {code}")
                    build = latest_build(worktree)
                    result.build_id = (build / "build_id.txt").read_text().strip()
                    result.image_hash = image_hash(build)
                except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
                    log.write(str(error) + "\n")
            result.build_seconds = (clock.now() - begin).total_seconds()
            if result.image_hash:
                if step.role != "anchor" and result.image_hash == previous_hash:
                    result.outcome = "skipped_same_image"
                else:
                    measure = clock.now()
                    result.outcome = "capture_failed"
                    try:
                        capture_args = SimpleNamespace(project=worktree, out=output / f"{index:0{STEP_INDEX_DIGITS}}-capture",
                                                       suite=args.suite, runs=1, timeout=CAPTURE_TIMEOUT_SECONDS,
                                                       wait=0)
                        record = run_flash(capture_args, seed=0, runner=runner)
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
                        failures += 1
                        if failures >= CAPTURE_FAILURE_LIMIT:
                            stop_reason = "capture_failures"
                    result.measure_seconds = (clock.now() - measure).total_seconds()
            if result.outcome in ("measured", "skipped_same_image", "build_failed"):
                if step.position >= positions.get(last_attempted, 0):
                    last_attempted = step.commit
            worst_step = max(worst_step, (clock.now() - begin).total_seconds())
            save()
        test_if_due(wait=True)
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
            save()
    return results


def plan(args, clock=None):
    clock = clock or Clock()
    now = clock.now()
    if args.queue_at:
        clock.sleep(max(0, (local_time(args.queue_at, now) - now).total_seconds()))
    tip = git(args.repo, "rev-parse", args.tip + "^{commit}")
    lanes = {}
    for lane, board in enumerate(args.boards):
        board = short_id(board)
        previous = read_last_night(args.data, board)
        steps = select_steps(args.repo, tip, previous, lane, len(args.boards), args.from_sha, args.max_steps)
        lanes[board] = dict(lane=f"{lane}/{len(args.boards)}", previous=previous,
                            steps=[asdict(step) for step in steps],
                            estimate=len(steps) * FIRST_STEP_SECONDS
                            + (SELFTEST_ALLOWANCE_SECONDS if args.selftest_board and board == short_id(args.selftest_board) else 0))
    write_json(Path(args.out), dict(tip=tip, night=clock.now().date().isoformat(),
                                    started=clock.now().isoformat(), lanes=lanes))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    waiting = commands.add_parser("wait-seconds")
    waiting.add_argument("--until", required=True)
    info = commands.add_parser("lane-info")
    info.add_argument("--plan", type=Path, required=True)
    info.add_argument("--board", required=True)
    planning = commands.add_parser("plan")
    planning.add_argument("--boards", nargs="+", required=True)
    planning.add_argument("--tip", default="origin/main")
    planning.add_argument("--queue-at")
    planning.add_argument("--selftest-board")
    running = commands.add_parser("run")
    running.add_argument("--plan", type=Path, required=True)
    running.add_argument("--board", required=True)
    running.add_argument("--lane", required=True)
    running.add_argument("--until", required=True)
    running.add_argument("--suite", nargs=3, action="append", default=[])
    running.add_argument("--selftest-at")
    running.add_argument("--selftest", nargs=argparse.REMAINDER)
    for command in (planning, running):
        command.add_argument("--repo", type=Path, default=REPO)
        command.add_argument("--data", type=Path, required=True)
        command.add_argument("--out", type=Path, required=True)
        command.add_argument("--from", dest="from_sha")
        command.add_argument("--max-steps", type=int)
    args = parser.parse_args()
    if args.command == "wait-seconds":
        now = Clock().now()
        print(max(0, int((local_time(args.until, now, next_occurrence=True) - now).total_seconds())))
        return 0
    if args.command == "lane-info":
        lanes = json.loads(args.plan.read_text(encoding="utf-8"))["lanes"]
        board = args.board if args.board in lanes else short_id(args.board)
        if board not in lanes:
            parser.error(f"unknown board in plan: {args.board}")
        lane = lanes[board]
        print(lane["lane"], lane["estimate"])
        return 0
    if args.command == "plan":
        plan(args)
        return 0
    if bool(args.selftest_at) != bool(args.selftest):
        parser.error("--selftest-at and --selftest must be supplied together")
    if not args.suite:
        parser.error("run requires at least one --suite")
    if not os.environ.get("_AUTANA_DEVICE_LOCK_TOKEN"):
        parser.error("run requires an enclosing autana lock run hold")
    planned = json.loads(args.plan.read_text(encoding="utf-8"))
    lane = planned["lanes"].get(args.board) or planned["lanes"][short_id(args.board)]
    if lane["lane"] != args.lane:
        parser.error("lane differs from the shared plan")
    args.previous, args.night, args.started = lane["previous"], planned["night"], planned["started"]
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
