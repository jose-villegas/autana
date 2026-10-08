"""Run CI's marked file-scoped text gates against an index snapshot."""
import os
import io
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

import yaml

from check_style_audit import BINARY_EXTENSIONS


def main():
    root = Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'],
                                       text=True).strip())
    changed = subprocess.check_output(
        ['git', 'diff', '--cached', '--name-only', '--diff-filter=ACMRD', '-z'], cwd=root)
    files = changed.decode('utf-8').strip('\0').split('\0') if changed else []
    if not files:
        return 0
    workflow = yaml.safe_load(subprocess.check_output(
        ['git', 'show', ':.github/workflows/comment-rules.yml'], cwd=root))
    commands = [shlex.split(step['run'])
                for job in workflow['jobs'].values() for step in job['steps']
                if step.get('env', {}).get('STAGED_TEXT_GATE') == 'true']
    if not commands:
        print('No staged text gates declared in comment-rules.yml', file=sys.stderr)
        return 1
    listing = subprocess.check_output(['git', 'ls-files', '--stage', '-z'], cwd=root)
    context = []
    for entry in listing.decode('utf-8').strip('\0').split('\0'):
        metadata, path = entry.split('\t', 1)
        if metadata.startswith('160000 ') or Path(path).suffix.lower() in BINARY_EXTENSIONS:
            continue
        context.append((metadata.split()[1], path))
    git_dir = subprocess.check_output(['git', 'rev-parse', '--absolute-git-dir'],
                                      cwd=root, text=True).strip()
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
        env = dict(os.environ, GIT_DIR=git_dir, GIT_WORK_TREE=str(snapshot))
        for command in commands:
            command[0] = sys.executable
            command[1] = str(snapshot / command[1])
            # CI requires the SDK; local machines without it still check the
            # project vocabulary and print the citation gate's skipped notice.
            command = [arg for arg in command if arg != '--require-idf']
            result = subprocess.run([*command, '--paths', *files], cwd=snapshot, env=env)
            status = status or result.returncode
    if status:
        print('Fix the reported staged text and git add <those files>.\n'
              'Run scripts/gates/check-text-rules-staged.sh to check again.\n'
              'To skip: git commit --no-verify', file=sys.stderr)
    return status


if __name__ == '__main__':
    sys.exit(main())
