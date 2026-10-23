"""Gate CI jobs using file access times, following fabricate's AtimesRunner class
(https://github.com/brushtyler/fabricate).
Linux relatime records the first read after arm resets access times; Docker
bind mounts share those inodes. Missing evidence runs the job.
"""
import argparse
import io
import itertools
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import time
import zipfile
from urllib.parse import quote

from github_api import call, pages
from inputs_recorded import latest_job_artifact

INDEX_SETTLE_SECONDS = 1
ARTIFACT_PREFIX = 'job-inputs-'
PR_FILES_LIMIT = 3000
MANIFEST_NAME = 'job-inputs.json'
MARKER_NAME = 'job-inputs-marker'
CANARY = '.git/job-inputs-canary'
RECORDER_FILES = {'scripts/ci/job_inputs.py', 'scripts/ci/github_api.py',
                  'scripts/ci/inputs_recorded.py'}
LOCAL_ACTION = re.compile(r'uses:\s*[\'\"]?(\./\.github/actions/[\w/-]+)')


def git(root, *args):
    return subprocess.run(['git', '-c', f'safe.directory={root.resolve()}', *args],
                          cwd=root, check=True, stdout=subprocess.PIPE).stdout


def tracked_paths(root):
    files = os.fsdecode(git(root, 'ls-files', '-z')).split('\0')
    files = {name for name in files if name}
    directories = {str(parent) for name in files for parent in PurePosixPath(name).parents}
    return files, directories


def arm(root, temporary):
    if os.environ.get('RUNNER_OS') != 'Linux':
        raise RuntimeError('job inputs require a Linux runner with working atime')
    git(root, 'config', 'core.trustctime', 'false')
    # A whole filesystem timestamp tick keeps freshly checked-out entries non-racy.
    time.sleep(INDEX_SETTLE_SECONDS)
    git(root, 'update-index', '--really-refresh')
    files, directories = tracked_paths(root)
    (root / CANARY).write_text('canary')
    for name in files | directories | {CANARY}:
        path = root / name
        try:
            stat = path.lstat()
            os.utime(path, ns=(0, stat.st_mtime_ns), follow_symlinks=False)
        except FileNotFoundError:
            continue
    # Filesystem timestamps can be coarser than the runner's wall clock.
    marker = temporary / MARKER_NAME
    marker.write_text('')
    marker.write_text(str(marker.stat().st_mtime_ns))


def workflow_inputs(root, workflow, job, files):
    text = (root / workflow).read_text()
    block = re.search(rf'^  {re.escape(job)}:\s*\n(.*?)(?=^  [\w-]+:|\Z)', text, re.M | re.S)
    if block is None:
        raise RuntimeError(f'cannot find job {job} in {workflow}')
    pending = LOCAL_ACTION.findall(block[1])
    included = {workflow}
    visited = set()
    while pending:
        action = pending.pop().removeprefix('./')
        if action in visited:
            continue
        visited.add(action)
        included.update(name for name in files if name.startswith(action + '/'))
        for extension in ('yml', 'yaml'):
            definition = root / action / f'action.{extension}'
            if definition.is_file():
                pending.extend(LOCAL_ACTION.findall(definition.read_text()))
    return included


def record(root, temporary, workflow, job):
    marker = int((temporary / MARKER_NAME).read_text())
    files, directories = tracked_paths(root)
    canary = root / CANARY
    canary.read_bytes()
    if canary.stat().st_atime_ns < marker:
        raise RuntimeError('job inputs canary was not recorded: filesystem atime does not work')
    observed = {}
    for kind, names in (('files', files), ('directories', directories)):
        observed[kind] = set()
        for name in names:
            try:
                if (root / name).lstat().st_atime_ns >= marker:
                    observed[kind].add(name)
            except FileNotFoundError:
                continue
    included = workflow_inputs(root, workflow, job, files)
    if not observed['files'] - included - RECORDER_FILES:
        raise RuntimeError(f'{job}: no tree input read besides canary and workflow files; do not gate this job')
    observed['files'].update(included)
    result = {kind: sorted(names) for kind, names in observed.items()}
    (temporary / MANIFEST_NAME).write_text(json.dumps(result, indent=2) + '\n')
    return result


def matching_paths(changes, manifest, base_files=()):
    files = set(manifest['files'])
    directories = set(manifest['directories'])
    base_files = set(base_files)
    base_directories = {parent for name in base_files for parent in PurePosixPath(name).parents}
    read_paths = [PurePosixPath(name) for name in (files & base_files) - RECORDER_FILES]
    matches = set()
    for change in changes:
        names = [change['filename']]
        if 'previous_filename' in change:
            names.append(change['previous_filename'])
        for name in names:
            path = PurePosixPath(name)
            ancestor = next((parent for parent in path.parents if parent in base_directories),
                            PurePosixPath('.'))
            if name in files or (change['status'] in {'added', 'removed', 'renamed'}
                    and str(ancestor) in directories):
                matches.add(name)
            if name == change['filename'] and change['status'] in {'added', 'renamed'}:
                # The job read files like this one in that part of the tree.
                if any(read.suffix == path.suffix and read.is_relative_to(ancestor) for read in read_paths):
                    matches.add(name)
    return sorted(matches)


def report(job, decision, reason):
    line = f'{job}: {decision} — {reason}'
    print(line)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as output:
            output.write(line + '\n\n')


def affected(workflow, jobs, pull, base_ref, request=call):
    changes = list(itertools.islice(pages(f'/pulls/{pull}/files', None, request), PR_FILES_LIMIT))
    capped = len(changes) >= PR_FILES_LIMIT
    base_files, _ = tracked_paths(Path('.'))
    base_files.difference_update(change['filename'] for change in changes
                                 if change['status'] in {'added', 'renamed'})
    base_files.update(change['filename'] for change in changes if change['status'] == 'removed')
    base_files.update(change['previous_filename'] for change in changes if 'previous_filename' in change)
    decisions = {}
    for job in jobs:
        name = f'{ARTIFACT_PREFIX}{workflow}-{job}'
        artifacts = () if capped else pages(f'/actions/artifacts?name={quote(name)}', 'artifacts', request)
        artifact = None if capped else latest_job_artifact(artifacts, name, base_ref, request)
        if capped:
            decision, reason = 'true', f'PR files API reached its {PR_FILES_LIMIT}-file cap'
        elif artifact is None:
            decision, reason = 'true', 'no unexpired manifest for the base branch'
            print(f'::notice::{job}: {reason}; running')
        else:
            data = request('GET', f"/actions/artifacts/{artifact['id']}/zip", raw=True)
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                manifest = json.loads(archive.read(MANIFEST_NAME))
            matches = matching_paths(changes, manifest, base_files)
            decision = str(bool(matches)).lower()
            reason = 'matches: ' + json.dumps(matches, ensure_ascii=False) if matches else 'no changed input'
        decisions[job] = decision
        report(job, decision, reason)
    return decisions


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('step', choices=('arm', 'record', 'affected'))
    parser.add_argument('--job', action='append', default=[])
    args = parser.parse_args(argv)
    root = Path(os.environ.get('GITHUB_WORKSPACE', '.'))
    temporary = Path(os.environ['RUNNER_TEMP'])
    workflow = os.environ['GITHUB_WORKFLOW_REF'].split('@', 1)[0].split('/', 2)[2]
    stem = PurePosixPath(workflow).stem
    if args.step == 'arm':
        arm(root, temporary)
    elif args.step == 'record':
        if len(args.job) != 1:
            parser.error('record requires one --job')
        record(root, temporary, workflow, args.job[0])
        with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
            output.write(f'name={ARTIFACT_PREFIX}{stem}-{args.job[0]}\n')
            output.write(f'path={temporary / MANIFEST_NAME}\n')
    else:
        event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
        decisions = affected(stem, args.job, event['pull_request']['number'], os.environ['GITHUB_BASE_REF'])
        with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
            for job, decision in decisions.items():
                output.write(f'{job}={decision}\n')


if __name__ == '__main__':
    main()
