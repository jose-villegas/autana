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
from capture import require_same_board, board_label
from layout_measure import autana_command, run_flash, run_stamped, REPORT_LINE, write_json
from perf_job import job_estimate
from nightly.lnt import lnt_report, night_manifest, read_last_night

IMAGE_HEADER_BYTES = 24
SEGMENT_HEADER_BYTES = 8
APP_DESC_OFFSET = IMAGE_HEADER_BYTES + SEGMENT_HEADER_BYTES
APP_DESC_BYTES = 256
IMAGE_TRAILER_BYTES = 48
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
    if len(app) < APP_DESC_OFFSET + APP_DESC_BYTES + IMAGE_TRAILER_BYTES:
        raise ValueError("application image is too short")
    assets = (directory / "assets.bin").read_bytes()
    image = app[:APP_DESC_OFFSET] + app[APP_DESC_OFFSET + APP_DESC_BYTES:-IMAGE_TRAILER_BYTES]
    return hashlib.sha256(image + assets).hexdigest()[:IMAGE_HASH_DIGITS]


def git(repo, *words):
    return subprocess.check_output(["git", "-C", str(repo), *words], text=True).strip()


def history_steps(repo, tip):
    history = git(repo, "log", "--first-parent", "--reverse", "--format=%H%x00%s", tip)
    steps = []
    for position, line in enumerate(history.splitlines(), start=1):
        commit, subject = line.split("\0", 1)
        pr = re.search(r"\(#(\d+)\)|Merge pull request #(\d+)", subject)
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
    anchor_position = commits.index(anchor) + 1 if anchor else 0
    selected = []
    for step in history:
        if step.commit == anchor:
            step.role = "anchor"
            selected.append(step)
        elif step.position > anchor_position and step.position % lanes == lane:
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
        # An explicit range supplies its own baseline candidates.
        steps = [step for step in steps if step.role == "anchor"]
        steps.extend(step for step in history[commits.index(first):] if step.position % lanes == lane)
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
    board = args.board
    previous = getattr(args, "previous", None) or {}
    machine = previous.get("machine")
    last_measured = previous.get("last_measured")
    previous_hash = previous.get("last_image_hash") or next(
        (row.get("image_hash") for row in reversed(previous.get("steps", [])) if row["outcome"] == "measured"), None)
    results, failures, worst_step, stop_reason = [], 0, FIRST_STEP_SECONDS, None
    selftest = dict(board=board, outcome="not_run", report=None, started=None, seconds=0)
    selftest_at = (local_time(args.selftest_at, night_started, next_occurrence=True)
                   if getattr(args, "selftest_at", None) else None)
    manifest = Path(args.data) / "scoped" / board / night / "night.json"

    def save():
        value = night_manifest(board, machine, night, args.lane, results, stop_reason, last_measured, selftest)
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
        summary = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary:
            with open(summary, "a", encoding="utf-8") as stream:
                stream.write(f"Self-test {board}: {selftest['outcome']}\n\n")
                report = selftest["report"]
                if report and Path(report).is_file():
                    stream.write(Path(report).read_text(encoding="utf-8"))
        save()

    save()
    with tempfile.TemporaryDirectory(prefix="nightly-", dir=output) as directory:
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
                        builds = list(worktree.glob("launcher/build*/build_id.txt"))
                        build = max(builds, key=lambda path: path.stat().st_mtime).parent
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
                            if not actual or board_label(actual).split()[0].upper() != board.upper():
                                raise ValueError("capture does not identify the requested board")
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
                            previous_hash, last_measured, failures = result.image_hash, step.commit, 0
                        except (OSError, RuntimeError, ValueError) as error:
                            (output / f"{index:0{STEP_INDEX_DIGITS}}-error.log").write_text(str(error), encoding="utf-8")
                            failures += 1
                            if failures >= CAPTURE_FAILURE_LIMIT:
                                stop_reason = "capture_failures"
                        result.measure_seconds = (clock.now() - measure).total_seconds()
                worst_step = max(worst_step, (clock.now() - begin).total_seconds())
                save()
            test_if_due(wait=True)
        finally:
            if worktree.exists():
                records = worktree / ".records/device"
                if records.exists():
                    shutil.copytree(records, output / "device", dirs_exist_ok=True)
                git(args.repo, "worktree", "remove", "--force", "--force", str(worktree))
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
        previous = read_last_night(args.data, board)
        steps = select_steps(args.repo, tip, previous, lane, len(args.boards), args.from_sha, args.max_steps)
        lanes[board] = dict(lane=f"{lane}/{len(args.boards)}", previous=previous,
                            steps=[asdict(step) for step in steps],
                            estimate=job_estimate(len(steps), len(steps) * len(args.suite),
                                                  CAPTURE_TIMEOUT_SECONDS, restore=False)
                            + len(steps) * FIRST_STEP_SECONDS
                            + (SELFTEST_ALLOWANCE_SECONDS if board == args.selftest_board else 0))
    write_json(Path(args.out), dict(tip=tip, night=clock.now().date().isoformat(),
                                    started=clock.now().isoformat(), lanes=lanes))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    waiting = commands.add_parser("wait-seconds")
    waiting.add_argument("--until", required=True)
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
    running.add_argument("--selftest-at")
    running.add_argument("--selftest", nargs=argparse.REMAINDER)
    for command in (planning, running):
        command.add_argument("--repo", type=Path, default=REPO)
        command.add_argument("--data", type=Path, required=True)
        command.add_argument("--out", type=Path, required=True)
        command.add_argument("--suite", nargs=3, action="append", default=[])
        command.add_argument("--from", dest="from_sha")
        command.add_argument("--max-steps", type=int)
    args = parser.parse_args()
    if args.command == "wait-seconds":
        now = Clock().now()
        print(max(0, int((local_time(args.until, now, next_occurrence=True) - now).total_seconds())))
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
    lane = planned["lanes"][args.board]
    if lane["lane"] != args.lane:
        parser.error("lane differs from the shared plan")
    args.previous, args.night, args.started = lane["previous"], planned["night"], planned["started"]
    steps = [Step(**step) for step in lane["steps"]]
    if args.from_sha is not None or args.max_steps is not None:
        index, count = map(int, args.lane.split("/"))
        steps = select_steps(args.repo, planned["tip"], args.previous, index, count, args.from_sha, args.max_steps)
    results = run_chain(steps, args)
    manifest = read_last_night(args.data, args.board)
    return int(any(result.outcome in ("build_failed", "capture_failed") for result in results)
               or manifest["selftest"]["outcome"] == "failed")


if __name__ == "__main__":
    sys.exit(main())
