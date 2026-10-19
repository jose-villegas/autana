"""Capture onset arms inside one caller-held autana lock on a hot-tunable image.

--set sound.declick_us=5000 --set sound.amp_mute=1 --set sound.dac_ramp=4
measures baseline, each setting separately, and their combination. All settings
return to zero at the end. The board argument identifies the held board.
"""
import argparse
from datetime import date
import json
import os
from pathlib import Path
import shutil
import subprocess

EVENTS = ('amp-on', 'start', 'stop', 'steal', 'amp-off')
DEFAULTS = {'sound.declick_us': 0, 'sound.amp_mute': 0, 'sound.dac_ramp': 0}


def arms(settings):
    selected = {}
    for setting in settings:
        name, value = setting.split('=', 1)
        if name not in DEFAULTS or name in selected:
            raise ValueError(f'unknown or repeated setting: {name}')
        selected[name] = int(value)
    result = {'baseline': DEFAULTS.copy()}
    for name, value in selected.items():
        result[name] = dict(DEFAULTS, **{name: value})
    if len(selected) > 1:
        result['combined'] = dict(DEFAULTS, **selected)
    return result


def capture(args):
    if not os.environ.get('_AUTANA_DEVICE_LOCK_TOKEN'):
        raise ValueError('run inside autana lock run; one hold covers every arm')
    if args.runs <= 0:
        raise ValueError('runs must be positive')
    settings = arms(args.set)
    executable = shutil.which(args.autana) or args.autana

    def run(*command):
        return subprocess.run([executable, "--board", args.board, *map(str, command)], check=True,
                              capture_output=True, text=True).stdout.strip()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('w', encoding='utf-8', newline='\n') as output:
        def record(arm, command, reply):
            output.write(f'{arm}\t{command}\t{reply}\n')
            output.flush()

        output.write(f'# autana sound-onset capture on {args.board}, {date.today()}, '
                     'one record per line: <arm>\t<command>\t<reply>\n')
        record('meta', 'status', json.dumps(json.loads(run('status', '--json'))))
        before = json.loads(run('buildid', '--json'))
        record('meta', 'buildid', json.dumps(before))
        record('meta', 'runs', str(args.runs))
        try:
            for arm, values in settings.items():
                for name, value in values.items():
                    reply = run('tune', name, value)
                    record(arm, f'tune {name} {value}', reply.replace('\n', ' '))
                    if f'{name}={value}' not in reply or 'TUNE_ERR' in reply:
                        raise ValueError(f'tunable not applied: {name}; use --hot-tunables')
                record(arm, 'settings', json.dumps(values, sort_keys=True))
                for event in EVENTS:
                    command = f'sound onset {event}'
                    for _ in range(args.runs):
                        reply = run(*command.split())
                        lines = reply.splitlines()
                        if len(lines) != 2 or lines[-1] != 'SOUND_END' or not lines[0].startswith('SOUND onset '):
                            raise ValueError(f'invalid onset reply: {reply}')
                        record(arm, command, lines[0])
        finally:
            for name, value in DEFAULTS.items():
                run('tune', name, value)
            record('meta', 'status', json.dumps(json.loads(run('status', '--json'))))
            after = json.loads(run('buildid', '--json'))
            record('meta', 'buildid', json.dumps(after))
        if before['build_id'] != after['build_id']:
            raise ValueError('build changed during capture')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', required=True)
    parser.add_argument('--runs', type=int, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--set', action='append', default=[])
    parser.add_argument('--autana', default='autana')
    args = parser.parse_args()
    try:
        capture(args)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(2, f'{error}\n')


if __name__ == '__main__':
    main()
