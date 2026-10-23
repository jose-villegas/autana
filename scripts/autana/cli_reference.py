#!/usr/bin/env python3
"""Write autana's command tables into docs/cli/ from COMMAND_GROUPS.

    python scripts/autana/cli_reference.py            write every table
    python scripts/autana/cli_reference.py --check    fail when one is stale

Each group's table sits in the page that owns it, between
`generated: cli-<group>` markers; the global flags sit in the README
under `cli-flags`. A group without a page, a page without its block, and
a block no group owns are errors: nothing falls back.
"""
import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "scripts/autana"), str(ROOT / "launcher/tools/render")]
import autana  # noqa: E402
from generated_blocks import blocks, replace_block  # noqa: E402

CHECK = "python scripts/autana/cli_reference.py --check"
FLAGS_BLOCK = "cli-flags"
FLAGS_PAGE = "docs/cli/README.md"
# The page that owns each command group's table.
GROUP_PAGES = {
    "lock": "docs/cli/Device-Lock.md",
    "build": "docs/cli/Build-and-Flash.md",
    "tests": "docs/cli/Tests.md",
    "watch": "docs/cli/Watch.md",
    "drive": "docs/cli/Drive.md",
    "tune": "docs/cli/Live-Tuning.md",
    "debug": "docs/cli/Debug.md",
    "docs": "docs/cli/Docs-Search.md",
}


def cell(text):
    return text.replace("|", "\\|")


def group_table(commands):
    rows = ["| Command | What it does |", "|---|---|"]
    for command in commands:
        for synopsis, summary in command.usages:
            rows.append(f"| `{cell('autana ' + synopsis)}` | {cell(summary)} |")
    return "\n".join(rows)


def flags_table():
    rows = ["| Flag | What it does | Commands |", "|---|---|---|"]
    for flag, summary, commands in autana.BOARD_FLAGS:
        rows.append(f"| `{flag}` | {cell(summary)} | {cell(commands)} |")
    return "\n".join(rows)


def tables():
    """(page, block, body) for everything this script owns."""
    keys = [key for key, _, _ in autana.COMMAND_GROUPS]
    missing = [key for key in keys if key not in GROUP_PAGES]
    extra = [key for key in GROUP_PAGES if key not in keys]
    if missing or extra:
        raise ValueError(f"GROUP_PAGES out of step with COMMAND_GROUPS: missing {missing}, unknown {extra}")
    found = [(GROUP_PAGES[key], f"cli-{key}", group_table(commands))
             for key, _, commands in autana.COMMAND_GROUPS]
    return found + [(FLAGS_PAGE, FLAGS_BLOCK, flags_table())]


def run(check):
    stale = []
    wanted = {}
    for page, block, body in tables():
        path = ROOT / page
        if not path.is_file():
            raise ValueError(f"{page}: page for {block} does not exist")
        if block not in blocks(path.read_text(encoding="utf-8")):
            raise ValueError(f"{page}: missing generated block {block}")
        wanted.setdefault(page, set()).add(block)
        if replace_block(path, block, body, check, CHECK):
            stale.append(f"{page}#{block}")
    for path in sorted((ROOT / "docs/cli").glob("*.md")):
        page = path.relative_to(ROOT).as_posix()
        for block in blocks(path.read_text(encoding="utf-8")):
            if block.startswith("cli-") and block not in wanted.get(page, ()):
                raise ValueError(f"{page}: block {block} belongs to no command group")
    return stale


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--check", action="store_true", help="fail when a table is stale; write nothing")
    args = parser.parse_args()
    try:
        stale = run(args.check)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    for name in stale:
        print(("stale " if args.check else "wrote ") + name)
    return int(args.check and bool(stale))


if __name__ == "__main__":
    raise SystemExit(main())
