"""Run CI's marked host tests once when a pushed range changes C or C++ source.

    printf '%s\n' "<git's ref updates>" | python scripts/gates/check_host_tests_push.py

Git supplies every ref update on stdin. A new branch counts from where it
leaves the remote's default branch (origin/HEAD); one sharing no history with
it counts every file it holds. A moved submodule counts as changed source:
its pin is code the tests build. The tests are the workflow steps marked
PUSH_TEST_GATE: "true" in the repository this runs in, run from its top in
the order the workflows list them.
"""
import pathlib
import subprocess
import sys

from c_comments import SOURCE_EXTENSIONS
from tracked import repository
from workflow_marks import is_workflow, marked_commands

ZERO = "0" * 40
GITLINK = "160000"
SKIP = "To skip: git push --no-verify"


def git(root, *args, stdin=None):
    return subprocess.run(["git", *args], cwd=root, input=stdin, capture_output=True, text=True)


def changes_source(root, updates):
    """True when a pushed range changes a source file or a submodule pin, None
    when one cannot be read."""
    empty = git(root, "hash-object", "-t", "tree", "--stdin", stdin="").stdout.strip()
    changed = False
    for line in updates:
        fields = line.split()
        if len(fields) < 4 or fields[1] == ZERO:
            continue
        local, remote = fields[1], fields[3]
        if remote == ZERO:
            base = git(root, "merge-base", "origin/HEAD", local)
            remote = base.stdout.strip() if base.returncode == 0 else empty
        # :<old mode> <new mode> <old sha> <new sha> <status>, a tab, the path
        diff = git(root, "diff", "--raw", "--no-renames", remote, local)
        if diff.returncode != 0:
            return None
        for entry in diff.stdout.splitlines():
            metadata, path = entry.split("\t", 1)
            modes = metadata.lstrip(":").split()[:2]
            changed = changed or GITLINK in modes or path.endswith(SOURCE_EXTENSIONS)
    return changed


def test_commands(root):
    # Only a push that changes source needs YAML.
    import yaml

    paths = git(root, "ls-files", "--", ".github/workflows").stdout.splitlines()
    workflows = [yaml.safe_load((root / path).read_text(encoding="utf-8")) for path in paths if is_workflow(path)]
    return [command for command, _ in marked_commands(workflows, "PUSH_TEST_GATE", ("true",))]


def main():
    root = pathlib.Path(repository())
    changed = changes_source(root, sys.stdin.read().splitlines())
    if changed is None:
        print(f"Cannot inspect pushed range. {SKIP}", file=sys.stderr)
        return 1
    if not changed:
        return 0
    try:
        commands = test_commands(root)
    except ValueError as error:
        print(f"{error}. {SKIP}", file=sys.stderr)
        return 1
    for command in commands:
        sys.stdout.flush()
        if subprocess.run(command, cwd=root).returncode != 0:
            print(f"Host tests failed ({' '.join(command[1:])}); fix them before pushing. {SKIP}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
