"""Run CI's marked host tests once when a pushed range changes C or C++ source.

    printf '%s\n' "<git's ref updates>" | python scripts/gates/check_host_tests_push.py

Git supplies every ref update on stdin. A new branch counts from where it
leaves origin/main; one sharing no history with main counts every file it
holds. The tests are the workflow steps marked PUSH_TEST_GATE: "true" in the
repository this runs in, run from its top in the order the workflows list
them; none marked runs nothing.
"""
import pathlib
import shutil
import subprocess
import sys

import yaml

from c_comments import SOURCE_EXTENSIONS
from tracked import repository
from workflow_marks import is_workflow, marked_steps

ZERO = "0" * 40
SKIP = "To skip: git push --no-verify"


def git(root, *args, stdin=None):
    return subprocess.run(["git", *args], cwd=root, input=stdin, capture_output=True, text=True)


def changes_source(root, updates):
    """True when a pushed range changes a source file, None when one cannot be read."""
    sources = [f"*{suffix}" for suffix in SOURCE_EXTENSIONS]
    empty = git(root, "hash-object", "-t", "tree", "--stdin", stdin="").stdout.strip()
    changed = False
    for line in updates:
        fields = line.split()
        if len(fields) < 4 or fields[1] == ZERO:
            continue
        local, remote = fields[1], fields[3]
        if remote == ZERO:
            base = git(root, "merge-base", "origin/main", local)
            remote = base.stdout.strip() if base.returncode == 0 else empty
        diff = git(root, "diff", "--name-only", remote, local, "--", *sources)
        if diff.returncode != 0:
            return None
        changed = changed or bool(diff.stdout.strip())
    return changed


def test_commands(root):
    paths = git(root, "ls-files", "--", ".github/workflows").stdout.splitlines()
    workflows = [yaml.safe_load((root / path).read_text(encoding="utf-8")) for path in paths if is_workflow(path)]
    return [command for command, value in marked_steps(workflows, "PUSH_TEST_GATE") if value == "true"]


def main():
    root = pathlib.Path(repository())
    changed = changes_source(root, sys.stdin.read().splitlines())
    if changed is None:
        print(f"Cannot inspect pushed range. {SKIP}", file=sys.stderr)
        return 1
    if not changed:
        return 0
    for command in test_commands(root):
        # Windows looks in System32 before PATH, which finds WSL's bash
        # before Git's; PATH's own lookup finds the shell this hook runs in.
        command = [shutil.which(command[0]) or command[0], *command[1:]]
        sys.stdout.flush()
        if subprocess.run(command, cwd=root).returncode != 0:
            print(f"Host tests failed ({' '.join(command[1:])}); fix them before pushing. {SKIP}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
