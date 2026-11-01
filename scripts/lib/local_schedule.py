"""Local start markers and hang guards shared by nightly jobs."""
import argparse
from datetime import datetime, timedelta, timezone
import os
import re
import subprocess
import sys
from pathlib import Path
import time
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))
from scripts.ci.github_api import PER_PAGE, call, pages
from device_lock import LockStore, board_read_root, heavy_root, write_json
from process_tree import launch_process_tree, stop_process_tree, close_process_tree

NIGHT_START = "01:00"
# Boards start after the GPU's night work: two hours of day board time when no GPU work is queued.
BOARD_START = "03:00"
STARTS = ("NIGHT_START", "BOARD_START")
# The board night starts no new lane after this local time on its marker's date; running lanes finish.
BOARD_LAST_START = "08:00"
# A lane is given only the steps its estimate fits before this local time on its marker's date. It is soft:
# a started lane still runs to completion under its hang guards.
BOARD_END = "10:00"
# Each local time setting's default and the latest hour it takes. A start is the early morning after the
# evening trigger; the cut-off stays before noon, so a daytime override's cut-off has passed and it starts
# no lane after its plan. The deadline may reach early afternoon.
LOCAL_TIMES = dict(NIGHT_START=(NIGHT_START, 5), BOARD_START=(BOARD_START, 5),
                   BOARD_LAST_START=(BOARD_LAST_START, 11), BOARD_END=(BOARD_END, 13))
NIGHT_ZONE = "UTC"
HOSTED_WAIT_SECONDS = 350 * 60
GPU_LABEL = "gpu"
QUEUE_POLL_SECONDS = 60
GPU_HOST_MARKER = "gpu-runner.json"
GPU_HOST_MARKER_DAYS = 14
GPU_NOT_ON_HOST = "GPU runner not on this host"
RECENT_RUNS_WINDOW = timedelta(days=2)
BAKES_WORKFLOW = "bakes-gpu.yml"
DOC_IMAGES_WORKFLOW = "doc-images-gpu.yml"
GPU_WORKFLOWS = (BAKES_WORKFLOW, DOC_IMAGES_WORKFLOW)
GPU_STAGE_HANG_SECONDS = {DOC_IMAGES_WORKFLOW: 14 * 60 * 60, BAKES_WORKFLOW: 4 * 60 * 60}
NIGHT_STEP_HANG_SECONDS = 23 * 60 * 60
GPU_PENDING_STATUSES = ("queued", "waiting", "in_progress")
DEFERRED = "GPU doc images deferred: bakes queued or running"
EXIT_DEFERRED = 3
EXIT_HUNG = 1
MARKER_KEY_FORMAT = "%Y-%m-%d-%H%M"


class Clock:
    def now(self):
        return datetime.now().astimezone()

    def sleep(self, seconds):
        time.sleep(seconds)


def local_time(name, value=None):
    """The hour and minute of the local time setting `name`: `value`, else its variable, else its constant."""
    default, latest = LOCAL_TIMES[name]
    value = value or os.environ.get(name) or default
    if not re.fullmatch(r"[0-9]{2}:[0-5][0-9]", value) or int(value[:2]) > latest:
        raise ValueError(f"{name} must be HH:MM in 00:00–{latest:02d}:59: {value!r}")
    return tuple(map(int, value.split(":")))


def night_zone():
    name = os.environ.get("NIGHT_ZONE") or NIGHT_ZONE
    try:
        return ZoneInfo(name)
    except (ZoneInfoNotFoundError, ValueError) as error:
        raise ValueError(f"NIGHT_ZONE must be an IANA name: {name!r}") from error


def next_marker(trigger, night_start=None, start="NIGHT_START"):
    hour, minute = local_time(start, night_start)
    zone = night_zone()
    date = trigger.astimezone(zone).date()
    candidate = datetime(date.year, date.month, date.day, hour, minute, tzinfo=zone)
    if candidate < trigger:
        date += timedelta(days=1)
        candidate = datetime(date.year, date.month, date.day, hour, minute, tzinfo=zone)
    return candidate


def on_marker_date(name, marker):
    """The local time setting `name` on `marker`'s date. The marker carries NIGHT_ZONE's offset, so the board
    machine needs no time zone data."""
    hour, minute = local_time(name)
    return marker.replace(hour=hour, minute=minute, second=0, microsecond=0)


def night_cutoff(marker):
    """When the board night that starts at `marker` stops starting lanes: BOARD_LAST_START on its date."""
    return on_marker_date("BOARD_LAST_START", marker)


def night_end(marker):
    """The deadline the board night that starts at `marker` sizes its lanes to: BOARD_END on its date."""
    return on_marker_date("BOARD_END", marker)


def trigger_time(event, schedule, now):
    if event != "schedule":
        return now
    minute, hour, *_ = schedule.split()
    slot = now.astimezone(timezone.utc).replace(hour=int(hour), minute=int(minute), second=0, microsecond=0)
    return slot - timedelta(days=1) if slot > now else slot


def refresh_gpu_host(runner, clock):
    write_json(heavy_root() / GPU_HOST_MARKER,
               {"runner": runner, "refreshed_at": clock.now().isoformat()})


def gpu_on_host(clock):
    try:
        root = heavy_root()
        marker = LockStore(root, board_observer=True).read_json(root / GPU_HOST_MARKER)
        if not isinstance(marker, dict) or not marker.get("runner"):
            return False
        age = clock.now() - datetime.fromisoformat(marker["refreshed_at"])
        return timedelta(0) <= age <= timedelta(days=GPU_HOST_MARKER_DAYS)
    except (OSError, KeyError, TypeError, ValueError):
        return False


def gpu_pending(workflows=GPU_WORKFLOWS, request=None):
    cutoff = (Clock().now().astimezone(timezone.utc) - RECENT_RUNS_WINDOW).strftime("%Y-%m-%dT%H:%M:%SZ")
    request = request or call
    return any(job.get("status") in GPU_PENDING_STATUSES and GPU_LABEL in job.get("labels", [])
               for workflow in workflows
               for run in request("GET", f"/actions/workflows/{workflow}/runs?created=>={cutoff}&per_page={PER_PAGE}")["workflow_runs"]
               if run["status"] != "completed"
               for job in pages(f"/actions/runs/{run['id']}/jobs", "jobs", request))


def wait_idle(busy, clock, idle_polls=1):
    idle = 0
    while True:
        idle = 0 if busy() else idle + 1
        if idle >= idle_polls:
            return True
        clock.sleep(QUEUE_POLL_SECONDS)


def gpu_ready(clock=None, board_held=None):
    clock = clock or Clock()
    if board_held is None:
        board_held = LockStore(board_read_root(), board_observer=True).any_board_held
    return wait_idle(board_held, clock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("marker", "wait", "ready", "start"))
    parser.add_argument("--start", choices=STARTS, default="NIGHT_START",
                        help="the local start a marker waits for")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    clock = Clock()
    if args.mode == "marker":
        now = clock.now()
        # In NIGHT_ZONE either way: the night's cut-off is read in the marker's offset.
        marker = now.astimezone(night_zone()) if os.environ.get("OVERRIDE") == "true" else next_marker(
            trigger_time(os.environ.get("GITHUB_EVENT_NAME"), os.environ.get("SCHEDULE", ""), now), start=args.start)
        print(marker.isoformat(), flush=True)
        if os.environ.get("GITHUB_OUTPUT"):
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                output.write("marker=" + marker.isoformat() + "\n")
        return 0
    if args.mode == "wait":
        wait_parser = argparse.ArgumentParser(description="Wait for a marker within the hosted job budget")
        wait_parser.add_argument("--until", required=True, type=datetime.fromisoformat)
        marker = wait_parser.parse_args(args.command).until
        deadline = min(marker, clock.now() + timedelta(seconds=HOSTED_WAIT_SECONDS))
        while (remaining := (deadline - clock.now()).total_seconds()) > 0:
            clock.sleep(min(QUEUE_POLL_SECONDS, remaining))
        return 0
    workflow = os.environ["GPU_WORKFLOW"]
    if args.mode == "ready":
        refresh_gpu_host(os.environ.get("RUNNER_NAME") or "local", clock)
        if workflow == DOC_IMAGES_WORKFLOW and gpu_pending((BAKES_WORKFLOW,)):
            print(DEFERRED)
            if os.environ.get("GITHUB_STEP_SUMMARY"):
                with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
                    summary.write(DEFERRED + "\n")
            return EXIT_DEFERRED
        gpu_ready(clock)
        return 0
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("start needs -- <command...>")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write("rendered=true\n")
    started, code = clock.now(), EXIT_HUNG
    process = None
    try:
        process = launch_process_tree(command)
        try:
            code = process.wait(timeout=GPU_STAGE_HANG_SECONDS[workflow])
        except subprocess.TimeoutExpired:
            print(f"GPU_STAGE_HANG_SECONDS expired for {workflow}", flush=True)
            stop_process_tree(process)
        return code
    finally:
        if process is not None:
            close_process_tree(process)
        marker = os.environ.get("MARKER") or started.isoformat()
        key = datetime.fromisoformat(marker).strftime(MARKER_KEY_FORMAT)
        write_json(heavy_root() / "night-steps" / key / (workflow + ".json"),
                   dict(workflow=workflow, run=os.environ.get("GITHUB_RUN_ID"), marker=marker,
                        started=started.isoformat(), ended=clock.now().isoformat(), exit=code))


if __name__ == "__main__":
    raise SystemExit(main())
