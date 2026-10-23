"""Local wall-clock deadlines shared by nightly jobs."""
import argparse
from datetime import datetime, timedelta
import os
import subprocess
import sys
from pathlib import Path
import time

NIGHT_START = "22:30"
NIGHT_END = "07:00"
QUEUE_POLL_SECONDS = 60
BAKES_ESTIMATE_SECONDS = 2 * 60 * 60
DOC_IMAGES_ESTIMATE_SECONDS = 2 * 60 * 60
BAKES_WORKFLOW = "bakes-gpu.yml"
DOC_IMAGES_WORKFLOW = "doc-images-gpu.yml"
GPU_ESTIMATES = {BAKES_WORKFLOW: BAKES_ESTIMATE_SECONDS,
                 DOC_IMAGES_WORKFLOW: DOC_IMAGES_ESTIMATE_SECONDS}
GPU_PENDING_STATUSES = ("queued", "waiting", "in_progress", "pending", "requested")
NEXT_NIGHT_AFTER = "12:00"
REFUSED = "GPU stage cannot fit in the local night window; dispatch with override to render now"
EXIT_REFUSED = 1
SKIPPED = "skipped: too late, next night renders it"
EXIT_SKIPPED = 3
DEFERRED = "GPU doc images deferred: bakes queued or running"


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


def wait_seconds(until, now, next_occurrence=False):
    return max(0, int((local_time(until, now, next_occurrence) - now).total_seconds()))


def night_window(start_at=None, until=NIGHT_END, scheduled=False, clock=None):
    start_at = start_at or os.environ.get("NIGHT_START") or NIGHT_START
    clock = clock or Clock()
    now = clock.now()
    start = local_time(start_at, now)
    end = local_time(until, now)
    if end <= start:
        if now < start:
            start -= timedelta(days=1)
        else:
            end += timedelta(days=1)
    if scheduled and now > local_time(NEXT_NIGHT_AFTER, now) and now >= end:
        start += timedelta(days=1)
        end += timedelta(days=1)
    if scheduled and now < start:
        clock.sleep((start - now).total_seconds())
        now = clock.now()
    return (start, end) if start <= now < end else None


def gpu_window(scheduled=False, override=False, clock=None):
    return override or night_window(scheduled=scheduled, clock=clock) is not None


def gpu_pending(workflows=tuple(GPU_ESTIMATES), request=None):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ci"))
    from github_api import call, pages
    request = request or call
    return any(run["status"] in GPU_PENDING_STATUSES
               for workflow in workflows
               for status in GPU_PENDING_STATUSES
               for run in pages(f"/actions/workflows/{workflow}/runs?status={status}",
                                "workflow_runs", request))


def gpu_estimate(workflow, request=None):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ci"))
    from github_api import call
    response = (request or call)(
        "GET", f"/actions/workflows/{workflow}/runs?branch=main&status=success&per_page=1")
    runs = response["workflow_runs"]
    if not runs:
        return GPU_ESTIMATES[workflow]
    run = runs[0]
    return (datetime.fromisoformat(run["updated_at"].replace("Z", "+00:00")) -
            datetime.fromisoformat(run["run_started_at"].replace("Z", "+00:00"))).total_seconds()


def wait_idle(busy, deadline, estimate, clock):
    while True:
        remaining = (deadline - clock.now()).total_seconds() - estimate
        if remaining < 0:
            return False
        pending = busy()
        remaining = (deadline - clock.now()).total_seconds() - estimate
        if remaining < 0:
            return False
        if not pending:
            return True
        if remaining <= 0:
            return False
        clock.sleep(min(QUEUE_POLL_SECONDS, remaining))


def gpu_queue(estimate, scheduled=False, override=False, clock=None, request=None):
    clock = clock or Clock()
    window = None if override else night_window(scheduled=scheduled, clock=clock)
    if not override and (window is None or (window[1] - clock.now()).total_seconds() < estimate):
        return SKIPPED if scheduled else REFUSED
    return DEFERRED if gpu_pending((BAKES_WORKFLOW,), request) else None


def gpu_ready(estimate, scheduled=False, override=False, clock=None,
              board_held=None):
    clock = clock or Clock()
    window = None if override else night_window(scheduled=scheduled, clock=clock)
    if not override and window is None:
        return False
    if board_held is None:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))
        from device_lock import LockStore, board_read_root
        board_held = LockStore(board_read_root()).any_board_held
    if override:
        from autana_config import DEFAULT_LOCK_WAIT_SECONDS
        deadline = clock.now() + timedelta(seconds=DEFAULT_LOCK_WAIT_SECONDS)
    else:
        deadline = window[1] - timedelta(seconds=estimate)
    return wait_idle(board_held, deadline, 0, clock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("gate", "queue", "ready", "estimate", "budget", "start"))
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    override = os.environ.get("OVERRIDE") == "true"
    scheduled = os.environ.get("GITHUB_EVENT_NAME") in ("schedule", "workflow_run")
    workflow = os.environ.get("GPU_WORKFLOW", DOC_IMAGES_WORKFLOW)
    wait_for_start = scheduled and os.environ.get("GPU_QUEUE_READY") != "true"
    if args.mode == "estimate":
        print(gpu_estimate(workflow))
        return 0
    if args.mode == "queue":
        message = gpu_queue(gpu_estimate(workflow), scheduled, override)
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write("gpu=" + str(message is None).lower() + "\n")
        if message:
            print(message)
            if os.environ.get("GITHUB_STEP_SUMMARY"):
                with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
                    summary.write(message + "\n")
        return EXIT_REFUSED if message == REFUSED else 0
    estimate = float(os.environ.get("GPU_ESTIMATE_SECONDS", "0"))
    clock = Clock()
    if args.mode == "ready":
        allowed = gpu_ready(estimate, wait_for_start, override, clock)
    else:
        window = None if override else night_window(scheduled=args.mode == "gate" and wait_for_start, clock=clock)
        allowed = override or window is not None and (window[1] - clock.now()).total_seconds() >= estimate
    if args.mode == "budget":
        from autana_config import DEFAULT_LOCK_WAIT_SECONDS
        print(DEFAULT_LOCK_WAIT_SECONDS if override else
              max(0, int((window[1] - clock.now()).total_seconds() - estimate)) if window else 0)
        return 0
    if not allowed:
        message = SKIPPED if scheduled else REFUSED
        print(message)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
                summary.write(message + "\n")
        if args.mode == "gate":
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                output.write("render=false\n")
            return EXIT_SKIPPED if scheduled else EXIT_REFUSED
        if scheduled:
            return EXIT_SKIPPED if args.mode == "ready" else 0
        return EXIT_REFUSED
    if args.mode in ("gate", "ready"):
        return 0
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("start needs -- <command...>")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write("rendered=true\n")
    return subprocess.call(command)


if __name__ == "__main__":
    raise SystemExit(main())
