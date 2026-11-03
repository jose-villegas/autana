"""Capture onset arms inside one caller-held autana lock on a hot-tunable image.

Pass each arm explicitly: --arm baseline:sound.declick_us=0
--arm declick:sound.declick_us=2000. The tunable returns to its firmware
default at the end. The board argument identifies the held board.
"""
import argparse
import ast
from datetime import date
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/lib"))
from console_protocol import reply_prefix
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
from frame_cost import US_PER_SECOND

SOUND_PREFIX, SOUND_UNTIL = reply_prefix("sound")


def read_string_array(header, name):
    """Read a literal string array, naming its owning header on failure."""
    header = Path(header)
    match = re.search(r"\b" + re.escape(name) + r"\s*\[\s*\]\s*=\s*\{([^}]*?)\}\s*;",
                      header.read_text(encoding="utf-8"), re.DOTALL)
    if match is None:
        raise RuntimeError(f"{header} declares no {name}")
    try:
        values = ast.literal_eval("[" + match[1] + "]")
        if not all(isinstance(value, str) for value in values):
            raise ValueError("non-string element")
        return tuple(values)
    except (ValueError, SyntaxError) as error:
        raise RuntimeError(f"{header}: {name} is not a literal string array") from error


EVENTS = read_string_array(ROOT / "launcher/main/sound/console/console_sound_parse.h", "console_sound_events")
DECLICK_TUNABLE = 'sound.declick_us'
ONSET_ATTEMPTS = 2


def arms(settings):
    result = {}
    for setting in settings:
        arm, assignment = setting.split(':', 1)
        name, value = assignment.split('=', 1)
        if not re.fullmatch(r'[A-Za-z0-9_-]+', arm) or arm == 'meta' or arm in result or name != DECLICK_TUNABLE:
            raise ValueError(f'unknown setting or invalid/repeated arm: {setting}')
        value = int(value)
        if not 0 <= value <= US_PER_SECOND:
            raise ValueError(f'de-click length out of range: {value}')
        result[arm] = {name: value}
    if not result:
        raise ValueError('at least one --arm is required')
    return result


def capture(args):
    if not os.environ.get('_AUTANA_DEVICE_LOCK_TOKEN'):
        raise ValueError('run inside autana lock run; one hold covers every arm')
    if args.runs <= 0:
        raise ValueError('runs must be positive')
    settings = arms(args.arm)
    executable = shutil.which(args.autana) or args.autana

    def run(*command):
        return subprocess.run([executable, "--board", args.board, *map(str, command)], check=True,
                              capture_output=True, text=True)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('w', encoding='utf-8', newline='\n') as output:
        def record(arm, command, reply):
            output.write(f'{arm}\t{command}\t{reply}\n')
            output.flush()

        output.write(f'# autana sound-onset capture on {args.board}, {date.today()}, '
                     'one record per line: <arm>\t<command>\t<reply>\n')
        record('meta', 'status', json.dumps(json.loads(run('status', '--json').stdout)))
        before = json.loads(run('buildid', '--json').stdout)
        record('meta', 'buildid', json.dumps(before))
        record('meta', 'runs', str(args.runs))
        try:
            for arm, values in settings.items():
                for name, value in values.items():
                    reply = run('tune', name, value).stdout.strip()
                    record(arm, f'tune {name} {value}', reply.replace('\n', ' '))
                    if f'{name}={value}' not in reply or 'TUNE_ERR' in reply:
                        raise ValueError(f'tunable not applied: {name}; use --hot-tunables')
                record(arm, 'settings', json.dumps(values, sort_keys=True))
                for event in EVENTS:
                    command = f'sound onset {event}'
                    for _ in range(args.runs):
                        for attempt in range(ONSET_ATTEMPTS):
                            try:
                                completed = run(*command.split())
                            except subprocess.CalledProcessError as error:
                                record(arm, f'failure {command}', json.dumps(
                                    {'attempt': attempt + 1, 'returncode': error.returncode,
                                     'stdout': error.stdout, 'stderr': error.stderr}))
                            else:
                                break
                        else:
                            continue
                        if completed.stderr:
                            record(arm, f'stderr {command}', json.dumps(completed.stderr))
                        reply = completed.stdout.strip()
                        lines = reply.splitlines()
                        if len(lines) != 2 or lines[-1] != SOUND_UNTIL[0] or not lines[0].startswith(SOUND_PREFIX + ' onset '):
                            raise ValueError(f'invalid onset reply: {reply}')
                        record(arm, command, lines[0])
        finally:
            run('tune', 'reset', DECLICK_TUNABLE)
            record('meta', 'status', json.dumps(json.loads(run('status', '--json').stdout)))
            after = json.loads(run('buildid', '--json').stdout)
            record('meta', 'buildid', json.dumps(after))
        if before['build_id'] != after['build_id']:
            raise ValueError('build changed during capture')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', required=True)
    parser.add_argument('--runs', type=int, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--arm', action='append', required=True)
    parser.add_argument('--autana', default='autana')
    args = parser.parse_args()
    try:
        capture(args)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(2, f'{error}\n')


if __name__ == '__main__':
    main()
