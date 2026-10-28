"""Command execution and line records for held audio captures."""
import argparse
from contextlib import contextmanager
from datetime import date
from functools import partial
from pathlib import Path
import json
import shlex
import shutil
import subprocess
import sys
import time

AUTANA_RUN_ATTEMPTS = 3
AUTANA_RUN_RETRY_SECONDS = 1


class AutanaRunError(subprocess.CalledProcessError):
    def __str__(self):
        return f"{super().__str__()} stderr: {self.stderr}"


def autana_run(command, *words):
    executable = shutil.which(command)
    command = [executable] if executable else shlex.split(command)
    if not command:
        raise ValueError("empty autana command")
    command[0] = shutil.which(command[0]) or command[0]
    for attempt in range(AUTANA_RUN_ATTEMPTS):
        try:
            return subprocess.run([*command, *map(str, words)], check=True,
                                  capture_output=True, text=True).stdout
        except subprocess.CalledProcessError as error:
            detail = " ".join((error.stderr or "").splitlines())
            print(f"autana attempt {attempt + 1}/{AUTANA_RUN_ATTEMPTS} failed: {detail}",
                  file=sys.stderr, flush=True)
            if attempt + 1 == AUTANA_RUN_ATTEMPTS:
                raise AutanaRunError(error.returncode, error.cmd, output=error.output,
                                     stderr=error.stderr) from error
            time.sleep(AUTANA_RUN_RETRY_SECONDS)


OUTPUT_FLAGS = ("-o", "--out")


def record_line(output, key, payload):
    output.write(f"{key}\t{payload}\n")
    output.flush()


def record_reply(output, runner, command, *words):
    reply = runner(command, *words).strip()
    payload = json.loads(reply) if words[0] in ("tone", "buildid") else reply
    record_line(output, " ".join(record_key(words)).removesuffix(" --json"),
                json.dumps(payload, allow_nan=False))
    return payload


def recorded_take(capture, words, reply):
    filename = words[words.index('-o') + 1]
    if Path(filename).name != filename or '\\' in filename:
        raise ValueError('take must be beside capture')
    return capture.parent / filename, json.loads(reply) if isinstance(reply, str) else reply


def record_key(words):
    """The words a capture records: an output path keeps only its file name, so no machine path lands in docs."""
    words = [str(word) for word in words]
    return [Path(word.replace("\\", "/")).name if i and words[i - 1] in OUTPUT_FLAGS else word
            for i, word in enumerate(words)]


def reset_tunables(record, owner, names):
    failures = []
    for name in names:
        try:
            record("tune", "reset", owner + "." + name)
        except (OSError, subprocess.CalledProcessError) as error:
            failures.append(error)
    if failures:
        raise RuntimeError(owner + " tunable reset failed") from failures[0]


def capture_parser(description, capture):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--autana", default="autana")
    parser.add_argument("--board", required=True)
    parser.add_argument("--out", type=Path, default=capture)
    return parser


@contextmanager
def capture_records(args, description, runner):
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="\n") as output:
        output.write(f"# autana --json replies on {args.board}, {date.today()}, {description}\n")
        record = partial(record_reply, output, runner, args.autana)
        record.note = lambda key, payload: record_line(output, key, json.dumps(payload, allow_nan=False))
        record("buildid", "--json")
        yield record
