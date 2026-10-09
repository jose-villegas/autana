"""Check generated Markdown blocks: a recorded SHA-256 against its body, a
named check command by running it from the repository root."""
import argparse
import pathlib
import shlex
import subprocess
import sys

from check_generated_files import INTERPRETERS
from tracked import tracked_files

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "launcher/tools/render"))
from generated_blocks import blocks, check_commands, verify


def run_check(root, command):
    """None when `command` exits 0 in `root`, else its last output lines."""
    try:
        words = shlex.split(command)
        if not words or words[0] not in INTERPRETERS:
            return f"names no interpreter of {sorted(INTERPRETERS)}"
        words[0] = INTERPRETERS[words[0]]
        result = subprocess.run(words, cwd=root, capture_output=True, text=True, check=False)
    except (ValueError, OSError) as error:
        return str(error)
    if result.returncode == 0:
        return None
    return " | ".join((result.stdout + result.stderr).strip().splitlines()[-5:]) or f"exit {result.returncode}"


def check(root):
    errors = []
    owners = {}
    commands = {}
    for name in tracked_files(root, ["*.md"]):
        try:
            text = (root / name).read_text(encoding="utf-8")
            for block in blocks(text):
                if block in owners:
                    errors.append(f"{name}#{block}: duplicate name, also in {owners[block]}")
                owners[block] = name
            errors.extend(f"{name}#{block}: generated body changed; run its generator"
                          for block in verify(text))
            for block, command in check_commands(text).items():
                commands.setdefault(command, []).append(f"{name}#{block}")
        except ValueError as error:
            errors.append(f"{name}: {error}")
    for command, named in commands.items():
        problem = run_check(root, command)
        if problem:
            errors.extend(f"{block}: `{command}` failed: {problem}" for block in named)
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(error)
    print(f"generated documentation: {len(errors)} errors")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
