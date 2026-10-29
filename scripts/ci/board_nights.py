"""Start one board night per board host: a nightly-board.yml run on each host label in BOARD_HOSTS.

Each host's run plans lanes for the boards on its own USB, so a board counts wherever it is plugged
in. Runs are separate so a host whose runner is offline holds only its own run; the next night's
run for that host replaces it. BOARD_HOSTS is a JSON list of runner labels, each carried by one
board host's runner beside `board`. Reads MARKER, FROM_SHA, MAX_STEPS, GITHUB_REF_NAME.
"""
import json
import os
import sys

from github_api import call

BOARD_NIGHT_WORKFLOW = 'nightly-board.yml'


def hosts(text):
    """The host labels BOARD_HOSTS lists; fails naming what is wrong rather than running no board."""
    try:
        labels = json.loads(text or '')
    except ValueError as error:
        raise SystemExit(f'BOARD_HOSTS is not a JSON list of runner labels: {error}')
    if not isinstance(labels, list) or not labels or not all(isinstance(label, str) and label for label in labels):
        raise SystemExit(f'BOARD_HOSTS must be a non-empty JSON list of runner labels, got {text!r}')
    if len(set(label.lower() for label in labels)) != len(labels):
        raise SystemExit(f'BOARD_HOSTS names a host twice: {text!r}')
    return labels


def start(labels, ref, marker, from_sha='', max_steps='', request=call):
    for label in labels:
        inputs = {'host': label, 'marker': marker}
        if from_sha:
            inputs['from'] = from_sha
        if max_steps:
            inputs['max_steps'] = max_steps
        request('POST', f'/actions/workflows/{BOARD_NIGHT_WORKFLOW}/dispatches', {'ref': ref, 'inputs': inputs})
        print(f'{label}: board night started for {marker}')


def main():
    start(hosts(os.environ.get('BOARD_HOSTS')), os.environ['GITHUB_REF_NAME'], os.environ['MARKER'],
          os.environ.get('FROM_SHA', ''), os.environ.get('MAX_STEPS', ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
