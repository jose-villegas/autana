"""Local wall-clock deadlines shared by nightly jobs."""
import argparse
from datetime import datetime, timedelta
import os
import subprocess
import time

GPU_START = "01:00"
GPU_END = "03:00"
GPU_NEXT_NIGHT_AFTER = "12:00"
REFUSED = "GPU stage renders 01:00-03:00 local time; dispatch with override to render now"
EXIT_REFUSED = 1
SKIPPED = "skipped: too late, next night renders it"
EXIT_SKIPPED = 3


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


def wait_seconds(until, now, next_occurrence=False, started=None):
    return max(0, int((local_time(until, started or now, next_occurrence) - now).total_seconds()))


def night_window(start_at, until, scheduled=False, clock=None):
    clock = clock or Clock()
    now = clock.now()
    start = local_time(start_at, now)
    end = local_time(until, now)
    if end <= start:
        if now < start:
            start -= timedelta(days=1)
        else:
            end += timedelta(days=1)
    if scheduled and now > local_time(GPU_NEXT_NIGHT_AFTER, now) and now >= end:
        start += timedelta(days=1)
        end += timedelta(days=1)
    if scheduled and now < start:
        clock.sleep((start - now).total_seconds())
        now = clock.now()
    return end if start <= now < end else None


def gpu_window(scheduled=False, override=False, clock=None):
    return override or night_window(GPU_START, GPU_END, scheduled, clock) is not None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("gate", "start"))
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    override = os.environ.get("OVERRIDE") == "true"
    scheduled = os.environ.get("GITHUB_EVENT_NAME") == "schedule"
    allowed = gpu_window(scheduled=args.mode == "gate" and scheduled,
                         override=override)
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
        return 0 if scheduled else EXIT_REFUSED
    if args.mode == "gate":
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
