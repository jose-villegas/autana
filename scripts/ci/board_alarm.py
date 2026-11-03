"""Keep one GitHub issue per CI alarm kind open while it holds, and check runner, USB, recovery and Pages evidence."""
import argparse
from datetime import datetime, timedelta, timezone
import io
import itertools
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
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'lib'))
from device_capture import board_steps_completed
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'launcher/tools/perf'))
from nightly.lnt import night_ran

# Every alarm issue carries ALARM_LABEL beside its kind's own label, so one query lists them all.
ALARM_LABEL = 'ci-alarm'
KINDS = {'down': ('board-ci-down', 'Board CI is down'),
         'red': ('board-ci-red', 'Board tests are red on main'),
         'pages': ('pages-down', 'Pages is down')}
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
# The boards expected on USB: those that ran a lane in any of the last FLEET_NIGHTS nights.
FLEET_NIGHTS = 3
# The run's error annotations an alarm quotes at most; the logs hold the rest.
ERROR_LINES = 20
PAGES_WORKFLOW = 'pages-trigger.yml'
PAGES_ENVIRONMENT = 'github-pages'
# The daily Pages run plus the lateness GitHub's scheduler has shown (6 h on 10-31).
PAGES_STALE = timedelta(hours=30)


def open_alarm(kind, request=call):
    return next((issue for issue in pages(f'/issues?state=open&labels={KINDS[kind][0]}', None, request)
                 if 'pull_request' not in issue), None)


def raise_alarm(kind, reason, run_url, request=call):
    """Open the kind's issue, or comment on the open one unless its last word already says the same."""
    label, title = KINDS[kind]
    workflow = os.environ.get('GITHUB_WORKFLOW', 'Board health')
    job = os.environ.get('GITHUB_JOB', 'check')
    said = f'{reason}\n\nWorkflow/job: {workflow} / {job}\n\nLogs: '
    issue = open_alarm(kind, request)
    if issue is not None:
        comments = list(pages(f"/issues/{issue['number']}/comments", None, request))
        if not (comments[-1]['body'] if comments else issue['body'] or '').startswith(said):
            request('POST', f"/issues/{issue['number']}/comments", {'body': said + run_url})
        return
    for name in (label, ALARM_LABEL):
        try:
            request('GET', f'/labels/{name}')
        except HTTPError as error:
            if error.code != 404:
                raise
            request('POST', '/labels', {'name': name})
    request('POST', '/issues', {'title': title, 'labels': [label, ALARM_LABEL], 'body': said + run_url})


def clear_alarm(kind, run_url, request=call):
    issue = open_alarm(kind, request)
    if issue is not None:
        number = issue['number']
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
    if missing:
        silent = f"; board hosts not answering: {', '.join(unreachable)}" if unreachable else ''
        problems.append('Boards missing from USB: ' + ', '.join(missing) + silent)
    elif unreachable:
        print('::notice::Board hosts not answering, with no board of the recent nights: ' + ', '.join(unreachable))
    return problems


def run_errors(run, request=call):
    """The error annotations of the run's failed jobs, at most ERROR_LINES of them."""
    lines = (f"{job['name']}: {note['message']}"
             for job in pages(f'/actions/runs/{run}/jobs', 'jobs', request) if job.get('conclusion') == 'failure'
             for note in pages(f"/check-runs/{job['id']}/annotations", None, request)
             if note.get('annotation_level') == 'failure')
    return list(itertools.islice(lines, ERROR_LINES))


def last_deploy(request):
    """When Pages last deployed: the newest github-pages deployment that went live, or None."""
    return next((stamp(deployment['created_at'])
                 for deployment in pages(f'/deployments?environment={PAGES_ENVIRONMENT}', None, request)
                 if any(status['state'] in ('success', 'inactive') for status in
                        pages(f"/deployments/{deployment['id']}/statuses", None, request))), None)


def pages_problems(request, now):
    """Why Pages is down: the latest finished main build of the site failed, or nothing deployed for PAGES_STALE."""
    run = next((run for run in pages(f'/actions/workflows/{PAGES_WORKFLOW}/runs?branch=main&status=completed',
                                     'workflow_runs', request)
                if run['event'] != 'pull_request' and run['conclusion'] != 'cancelled'), None)
    problems = [f"Pages build {run['conclusion']}: {run['html_url']}"] if run and run['conclusion'] != 'success' else []
    deployed = last_deploy(request)
    if deployed is None or now - deployed > PAGES_STALE:
        problems.append(f'No Pages deploy within {PAGES_STALE}'
                        + (f'; the last went live {deployed:%Y-%m-%d %H:%M} UTC' if deployed else ''))
    return problems


def fleet(directory):
    """The boards that ran a lane in any of the last FLEET_NIGHTS nights on perf-data."""
    manifests = list((Path(directory) / 'scoped').glob('*/*/night.json'))
    recent = sorted({path.parent.name for path in manifests})[-FLEET_NIGHTS:]
    return sorted({path.parent.parent.name for path in manifests if path.parent.name in recent})


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
    raising = commands.add_parser('raise')
    raising.add_argument('--kind', required=True, choices=KINDS)
    raising.add_argument('--errors', action='store_true', help="quote this run's error annotations")
    reason = raising.add_mutually_exclusive_group(required=True)
    reason.add_argument('--reason')
    reason.add_argument('--reason-file', type=Path)
    commands.add_parser('clear').add_argument('--kind', required=True, choices=KINDS)
    commands.add_parser('pages', help='raise or clear the pages alarm from the site\'s builds and deploys')
    health = commands.add_parser('check')
    health.add_argument('--lanes-from', required=True, type=Path)
    health.add_argument('--devices', required=True, type=Path)
    health.add_argument('--reason-file', required=True, type=Path)
    ran = commands.add_parser('ran', help="print boards_ran=true|false for a board-steps.sh log")
    ran.add_argument('--log', required=True, type=Path)
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
        errors = run_errors(os.environ['GITHUB_RUN_ID'], call) if args.errors else []
        raise_alarm(args.kind, reason + ''.join(f'\n- {line}' for line in errors), run_url())
    elif args.command == 'clear':
        clear_alarm(args.kind, run_url())
    elif args.command == 'pages':
        problems = pages_problems(call, datetime.now(timezone.utc))
        if problems:
            raise_alarm('pages', '\n'.join(problems), run_url())
        else:
            clear_alarm('pages', run_url())
    elif args.command == 'ran':
        lines = args.log.read_text(encoding='utf-8', errors='replace').splitlines() if args.log.is_file() else []
        print(f'boards_ran={str(board_steps_completed(lines)).lower()}')
    elif args.command == 'probe':
        return probe_devices(args.devices, args.reason_file)
    else:
        if not nights_enabled(call):
            return 0
        problems = check(call, datetime.now(timezone.utc), fleet(args.lanes_from),
                         *read_plugged(args.devices), night_ends(args.lanes_from))
        if problems:
            reason = '\n'.join(problems)
            args.reason_file.write_text(reason, encoding='utf-8')
            print(reason, file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
