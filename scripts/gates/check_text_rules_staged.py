"""Run CI's marked gates against an index snapshot.

The gates are the steps of the staged .github/workflows/ files that carry
STAGED_TEXT_GATE, in whichever repository this runs in.

A step marked STAGED_TEXT_GATE: "true" is file-scoped: it gets --paths and
the staged files. One marked "snapshot" checks the whole tree (a generated
catalogue against its sources, say) and runs on the snapshot as it is.
"""
import os
import io
from pathlib import Path
import subprocess
import sys
import tempfile

import yaml

from check_style_audit import BINARY_EXTENSIONS, DATA_EXTENSIONS
from tracked import repository
from workflow_marks import is_workflow, marked_commands


def main():
    root = Path(repository())
    changed = subprocess.check_output(
        ['git', 'diff', '--cached', '--name-only', '--diff-filter=ACMRD', '-z'], cwd=root)
    files = changed.decode('utf-8').strip('\0').split('\0') if changed else []
    if not files:
        return 0
    listing = subprocess.check_output(['git', 'ls-files', '--stage', '-z'], cwd=root)
    context = []
    for entry in listing.decode('utf-8').strip('\0').split('\0'):
        metadata, path = entry.split('\t', 1)
        suffix = Path(path).suffix.lower()
        if metadata.startswith('160000 ') or (suffix in BINARY_EXTENSIONS - DATA_EXTENSIONS):
            continue
        context.append((metadata.split()[1], path))
    workflows = [yaml.safe_load(subprocess.check_output(['git', 'show', f':{path}'], cwd=root))
                 for _, path in context if is_workflow(path)]
    try:
        commands = [(command, value == 'true') for command, value
                    in marked_commands(workflows, 'STAGED_TEXT_GATE', ('true', 'snapshot'))]
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    # Absolute both: git gives a commit hook GIT_INDEX_FILE=.git/index, which
    # the gates, run in the snapshot, would read as an index that is not there.
    git_dir, index = subprocess.check_output(
        ['git', 'rev-parse', '--absolute-git-dir', '--path-format=absolute', '--git-path', 'index'],
        cwd=root, text=True).split()
    status = 0
    with tempfile.TemporaryDirectory(prefix='autana-staged-') as folder:
        snapshot = Path(folder)
        # Context comes from the same index as the selected files: header pairs,
        # app names and cited symbols must describe the commit, too.
        blobs = io.BytesIO(subprocess.check_output(['git', 'cat-file', '--batch'], cwd=root,
                           input=('\n'.join(sha for sha, _ in context) + '\n').encode()))
        for _, path in context:
            size = int(blobs.readline().split()[2])
            target = snapshot / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(blobs.read(size))
            blobs.read(1)
        (snapshot / 'launcher/main/apps').mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, GIT_DIR=git_dir, GIT_INDEX_FILE=index, GIT_WORK_TREE=str(snapshot))
        # The gates read the snapshot and write nothing, so they run side by
        # side: the hook takes as long as the slowest, not their sum. Each
        # one's output is printed whole once it finishes.
        running = []
        for command, scoped in commands:
            # The commit's own gate when it holds one; a submodule's, which
            # no snapshot holds, runs from the checked-out pin.
            gate = snapshot / command[1]
            command[1] = str(gate if gate.exists() else root / command[1])
            # CI requires the SDK; local machines without it still check the
            # project vocabulary and print the citation gate's skipped notice.
            command = [arg for arg in command if arg != '--require-idf']
            selected = ['--paths', *files] if scoped else []
            running.append(subprocess.Popen([*command, *selected], cwd=snapshot, env=env,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT))
        for gate in running:
            output, _ = gate.communicate()
            sys.stdout.buffer.write(output)
            status = status or gate.returncode
        sys.stdout.flush()
    if status:
        print('Fix the reported staged text, or regenerate what is reported stale, and git add <those files>.\n'
              'Run scripts/gates/check-text-rules-staged.sh to check again.\n'
              'To skip: git commit --no-verify', file=sys.stderr)
    return status


if __name__ == '__main__':
    sys.exit(main())
