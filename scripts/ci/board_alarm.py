"""Keep one board CI outage issue and check runner, USB and recovery evidence."""
import argparse
from datetime import datetime, timedelta, timezone
import io
import json
import os
from pathlib import Path
import sys
import time
from urllib.error import HTTPError
import zipfile

from github_api import call, pages

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'device'))
from device_lock import short_id
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'launcher/tools/perf'))
from nightly.lnt import night_ran

LABEL = 'board-ci-down'
QUEUED_LIMIT = timedelta(minutes=30)
SUCCESS_WINDOW = timedelta(hours=24)
NIGHTLY_WORKFLOW = 'nightly-perf.yml'
BOARD_WORKFLOW = 'board-ci.yml'
BOARD_LABEL = 'board'
CLEAR_STEP = 'Clear the board-ci-down alarm'
PROBE_JOB = 'boards'
DEVICES_FILE = 'devices.json'
ARTIFACT_PREFIX = 'board-health-devices'
POLL_SECONDS = 30
FAILURE_REASON = 'Board health check failed; see the job logs'


def open_alarm(request=call):
    return next((issue['number'] for issue in pages(
        f'/issues?state=open&labels={LABEL}', None, request) if 'pull_request' not in issue), None)


def raise_alarm(reason, run_url, request=call):
    workflow = os.environ.get('GITHUB_WORKFLOW', 'Board health')
    job = os.environ.get('GITHUB_JOB', 'check')
    body = f'{reason}\n\nWorkflow/job: {workflow} / {job}\n\nLogs: {run_url}'
    number = open_alarm(request)
    if number is not None:
        request('POST', f'/issues/{number}/comments', {'body': body})
        return
    try:
        request('GET', f'/labels/{LABEL}')
    except HTTPError as error:
        if error.code != 404:
            raise
        request('POST', '/labels', {'name': LABEL})
    request('POST', '/issues', {'title': 'Board CI is down', 'labels': [LABEL], 'body': body})


def clear_alarm(run_url, request=call):
    number = open_alarm(request)
    if number is not None:
        request('POST', f'/issues/{number}/comments', {'body': f'Recovered: {run_url}'})
        request('PATCH', f'/issues/{number}', {'state': 'closed'})


def stamp(text):
    return datetime.fromisoformat(text.replace('Z', '+00:00'))


def nights_enabled(request):
    active = request('GET', f'/actions/workflows/{NIGHTLY_WORKFLOW}')['state'] == 'active'
    if not active:
        print('::notice::Board health checks paused: nightly workflow is disabled')
    return active


def night_ends(directory):
    """When each nightly lane on perf-data that ran to the end finished, whatever its tests reported."""
    for path in (Path(directory) / 'scoped').glob('*/*/night.json'):
        manifest = json.loads(path.read_text(encoding='utf-8'))
        if night_ran(manifest):
            yield datetime.fromisoformat(manifest['ended'])


def last_board_success(request, now, nights=()):
    """The latest board success: a nightly lane's end from `nights`, or a board CI run that cleared the alarm."""
    successes = list(nights)
    cutoff = (now - SUCCESS_WINDOW).astimezone(timezone.utc).isoformat().replace('+00:00', 'Z')
    for run in pages(f'/actions/workflows/{BOARD_WORKFLOW}/runs?status=completed&created=>={cutoff}',
                     'workflow_runs', request):
        jobs = list(pages(f"/actions/runs/{run['id']}/jobs", 'jobs', request))
        cleared = any(job['name'] == 'alarm' and any(
            step['name'] == CLEAR_STEP and step['conclusion'] == 'success'
            for step in job.get('steps', [])) for job in jobs)
        if cleared:
            successes.extend(stamp(job['completed_at']) for job in jobs
                             if BOARD_LABEL in job.get('labels', []) and job.get('conclusion') == 'success')
    return max((success for success in successes if success <= now), default=None)


def stuck_board_jobs(request, now):
    queued = {}
    running = False
    for status in ('queued', 'in_progress'):
        for run in pages(f'/actions/runs?status={status}', 'workflow_runs', request):
            for job in pages(f"/actions/runs/{run['id']}/jobs", 'jobs', request):
                if BOARD_LABEL not in job.get('labels', []):
                    continue
                running |= job['status'] == 'in_progress'
                if job['status'] == 'queued':
                    created = datetime.fromisoformat(job['created_at'].replace('Z', '+00:00'))
                    if now - created > QUEUED_LIMIT:
                        queued[job['html_url']] = f"Board job queued beyond {QUEUED_LIMIT}: {job['name']} {job['html_url']}"
    return [] if running else list(queued.values())


def missing_boards(lanes, plugged):
    return sorted(set(lanes) - set(plugged))


def night_running(request):
    running = any(job.get('status') == 'in_progress' and BOARD_LABEL in job.get('labels', [])
                  for run in pages(f'/actions/workflows/{NIGHTLY_WORKFLOW}/runs?status=in_progress',
                                   'workflow_runs', request)
                  if run.get('status') == 'in_progress'
                  for job in pages(f"/actions/runs/{run['id']}/jobs", 'jobs', request))
    if running:
        print('::notice::The night is still running')
    return running


def check(request, now, lanes, plugged, unreachable=(), nights=()):
    if night_running(request):
        return []
    problems = []
    success = last_board_success(request, now, nights)
    if success is None or now - success > SUCCESS_WINDOW:
        problems.append(f'No board success within {SUCCESS_WINDOW}')
    problems.extend(stuck_board_jobs(request, now))
    missing = missing_boards(lanes, plugged)
    if missing and unreachable:
        # A board on a host that did not answer may still be plugged in there.
        print('::notice::Board hosts not answering: ' + ', '.join(unreachable)
              + '; not checked: ' + ', '.join(missing))
    elif missing:
        problems.append('Boards missing from USB: ' + ', '.join(missing))
    return problems


def latest_lanes(directory):
    manifests = list((Path(directory) / 'scoped').glob('*/*/night.json'))
    latest = max((path.parent.name for path in manifests), default=None)
    return sorted(path.parent.parent.name for path in manifests if path.parent.name == latest)


def read_plugged(path):
    """The plugged boards' short ids and the board hosts that did not answer."""
    listing = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    return [short_id(board['board']) for board in listing['boards'] if board['port']], listing.get('unreachable', [])


def run_url():
    return (f"{os.environ['GITHUB_SERVER_URL']}/{os.environ['GITHUB_REPOSITORY']}"
            f"/actions/runs/{os.environ['GITHUB_RUN_ID']}")


def probe_devices(destination, reason_file, request=call, clock=time.monotonic, sleep=time.sleep):
    enabled = nights_enabled(request) and not night_running(request)
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
            output.write(f'enabled={str(enabled).lower()}\n')
    if not enabled:
        return 0
    run = os.environ['GITHUB_RUN_ID']
    attempt = os.environ.get('GITHUB_RUN_ATTEMPT', '1')
    deadline = clock() + QUEUED_LIMIT.total_seconds()
    while True:
        jobs = pages(f'/actions/runs/{run}/attempts/{attempt}/jobs', 'jobs', request)
        job = next((job for job in jobs if job['name'] == PROBE_JOB), None)
        if job and job['status'] == 'completed':
            if job['conclusion'] != 'success':
                reason = f"Board health USB probe {job['conclusion']}"
                break
            artifacts = pages(f'/actions/runs/{run}/artifacts', 'artifacts', request)
            artifact = next((item for item in artifacts if not item['expired'] and
                             item['name'] == f'{ARTIFACT_PREFIX}-{attempt}'), None)
            if artifact is None:
                reason = 'Board health USB probe has no devices artifact'
                break
            payload = request('GET', f"/actions/artifacts/{artifact['id']}/zip", raw=True)
            with zipfile.ZipFile(io.BytesIO(payload)) as archive:
                Path(destination).write_bytes(archive.read(DEVICES_FILE))
            return 0
        remaining = deadline - clock()
        if remaining <= 0:
            reason = ('board runner offline or idle' if not job or job['status'] == 'queued'
                      else f'Board health USB probe did not finish within {QUEUED_LIMIT}')
            break
        sleep(min(POLL_SECONDS, remaining))
    Path(reason_file).write_text(reason, encoding='utf-8')
    print(reason, file=sys.stderr)
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    reason = commands.add_parser('raise').add_mutually_exclusive_group(required=True)
    reason.add_argument('--reason')
    reason.add_argument('--reason-file', type=Path)
    commands.add_parser('clear')
    health = commands.add_parser('check')
    health.add_argument('--lanes-from', required=True, type=Path)
    health.add_argument('--devices', required=True, type=Path)
    health.add_argument('--reason-file', required=True, type=Path)
    probe = commands.add_parser('probe')
    probe.add_argument('--devices', required=True, type=Path)
    probe.add_argument('--reason-file', required=True, type=Path)
    args = parser.parse_args(argv)
    if args.command == 'raise':
        reason = args.reason
        if args.reason_file is not None:
            try:
                reason = args.reason_file.read_text(encoding='utf-8')
            except FileNotFoundError:
                reason = FAILURE_REASON
        raise_alarm(reason, run_url())
    elif args.command == 'clear':
        clear_alarm(run_url())
    elif args.command == 'probe':
        return probe_devices(args.devices, args.reason_file)
    else:
        if not nights_enabled(call):
            return 0
        problems = check(call, datetime.now(timezone.utc), latest_lanes(args.lanes_from),
                         *read_plugged(args.devices), night_ends(args.lanes_from))
        if problems:
            reason = '\n'.join(problems)
            args.reason_file.write_text(reason, encoding='utf-8')
            print(reason, file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
