"""Local wall-clock deadlines shared by nightly jobs."""
import argparse
from datetime import datetime, timedelta, timezone
import os
import re
import subprocess
import sys
from pathlib import Path
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))
from scripts.ci.github_api import PER_PAGE, call
from scripts.lib.autana_config import DEFAULT_LOCK_WAIT_SECONDS
from device_lock import LockStore, board_read_root

NIGHT_START = "22:30"
NIGHT_END = "07:00"
QUEUE_POLL_SECONDS = 60
RECENT_RUNS_WINDOW = timedelta(days=2)
BAKES_WORKFLOW = "bakes-gpu.yml"
DOC_IMAGES_WORKFLOW = "doc-images-gpu.yml"
# Doc images uses the measured 3–7 h worst case; bakes has a 2 h allowance.
GPU_ESTIMATES = {BAKES_WORKFLOW: 2 * 60 * 60, DOC_IMAGES_WORKFLOW: 7 * 60 * 60}
EVENING_START = "18:00"
GPU_PENDING_STATUSES = ("queued", "waiting", "in_progress", "pending", "requested")
# The 16:00 UTC cron arrives at 17:00 in winter, within 7 h of 23:59.
MAX_EVENING_WAIT = 7 * 60 * 60
PENDING = "pending: renders tonight"
NEXT_NIGHT_AFTER = "12:00"
REFUSED = "GPU stage cannot fit in the local night window; dispatch with override to render now"
EXIT_REFUSED = 1
SKIPPED = "skipped: too late, next night renders it"
EXIT_SKIPPED = 3
BOARDS_BUSY = "GPU stage boards busy: timed out waiting for active board work"
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
    configured_start = os.environ.get("NIGHT_START") or NIGHT_START
    if not re.fullmatch(r"(?:1[89]|2[0-3]):[0-5][0-9]", configured_start):
        raise ValueError(f"NIGHT_START must be HH:MM in {EVENING_START}–23:59: {configured_start!r}")
    start_at = start_at or configured_start
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
    cutoff = (Clock().now().astimezone(timezone.utc) - RECENT_RUNS_WINDOW).strftime("%Y-%m-%dT%H:%M:%SZ")
    request = request or call
    return any(run["status"] in GPU_PENDING_STATUSES
               for workflow in workflows
               for run in request("GET", f"/actions/workflows/{workflow}/runs?created=>={cutoff}&per_page={PER_PAGE}")["workflow_runs"])


def wait_idle(busy, deadline, estimate, clock, idle_polls=1):
    idle = 0
    while True:
        pending = busy()
        remaining = (deadline - clock.now()).total_seconds() - estimate
        if remaining <= 0:
            return False
        idle = 0 if pending else idle + 1
        if idle >= idle_polls:
            return True
        clock.sleep(min(QUEUE_POLL_SECONDS, remaining))


def gpu_queue(estimate, scheduled=False, override=False, clock=None, request=None):
    clock = clock or Clock()
    while True:
        window = night_window(clock=clock)
        if gpu_pending((BAKES_WORKFLOW,), request):
            return DEFERRED
        if override or window is not None:
            break
        if not scheduled:
            return REFUSED
        start = local_time(os.environ.get("NIGHT_START") or NIGHT_START, clock.now(), next_occurrence=True)
        remaining = (start - clock.now()).total_seconds()
        if remaining > MAX_EVENING_WAIT:
            return PENDING
        clock.sleep(min(QUEUE_POLL_SECONDS, remaining))
    if not override and (window[1] - clock.now()).total_seconds() < estimate:
        return SKIPPED if scheduled else REFUSED
    return None


def gpu_ready(estimate, scheduled=False, override=False, clock=None,
              board_held=None):
    clock = clock or Clock()
    window = None if override else night_window(scheduled=scheduled, clock=clock)
    if not override and window is None:
        return False
    if board_held is None:
        board_held = LockStore(board_read_root()).any_board_held
    if override:
        deadline = clock.now() + timedelta(seconds=DEFAULT_LOCK_WAIT_SECONDS)
    else:
        deadline = window[1] - timedelta(seconds=estimate)
    return wait_idle(board_held, deadline, 0, clock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("gate", "ready", "budget", "start"))
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    override = os.environ.get("OVERRIDE") == "true"
    scheduled = os.environ.get("GITHUB_EVENT_NAME") in ("schedule", "workflow_run")
    workflow = os.environ["GPU_WORKFLOW"]
    estimate = GPU_ESTIMATES[workflow]
    clock = Clock()
    message = gpu_queue(estimate, scheduled, override, clock) if args.mode == "ready" and workflow == DOC_IMAGES_WORKFLOW else None
    if args.mode == "ready":
        allowed = message is None and gpu_ready(estimate, scheduled, override, clock)
    else:
        window = None if override else night_window(scheduled=args.mode == "gate" and scheduled, clock=clock)
        allowed = override or window is not None and (window[1] - clock.now()).total_seconds() >= estimate
    if args.mode == "budget":
        print(DEFAULT_LOCK_WAIT_SECONDS if override else
              max(0, int((window[1] - clock.now()).total_seconds() - estimate)) if window else 0)
        return 0
    if not allowed:
        message = message or (BOARDS_BUSY if override else SKIPPED if scheduled else REFUSED)
        print(message)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
                summary.write(message + "\n")
        if args.mode == "gate":
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                output.write("render=false\n")
            return EXIT_SKIPPED if scheduled else EXIT_REFUSED
        if scheduled or message in (DEFERRED, PENDING):
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
