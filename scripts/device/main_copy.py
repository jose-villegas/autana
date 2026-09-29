"""Run the main checkout's copy of a board tool instead of this one.

Every worktree carries its own scripts/device/, but the lock they share is one
set of files, so two versions of the lock code on one machine can each believe
they hold the board. A board tool started from a worktree therefore hands its
command line to the same-named tool in the main checkout and exits with its
status. AUTANA_DEVICE_TOOLS=here runs this copy instead, for working on the
tools themselves.

`--worktree BRANCH` (docs/tools/Autana-CLI.md) always creates its worktree
under the main checkout's `.claude/worktrees/<name>/`, so that layout alone
names the main checkout - no git call needed. `--worktree PATH` can point
anywhere, so a worktree outside that layout still falls back to asking git
(the parent of its common directory); this is the one place left that runs
git, and only when the path-based answer does not apply.
"""

import os
import subprocess
import sys
from pathlib import Path


def by_worktree_convention(here):
    """The main checkout for `here`, read off its path alone, when `here` sits
    under <main>/.claude/worktrees/<name>/... - the layout every worktree
    autana itself creates has. None when `here` is not in that shape, so the
    caller falls back to asking git for a worktree placed anywhere else."""
    parts = here.parts
    for i in range(len(parts) - 2):
        if parts[i] == ".claude" and parts[i + 1] == "worktrees":
            return Path(*parts[:i]).joinpath(*parts[i + 3:])
    return None


def by_git_worktree(here):
    """The main checkout for `here`, asked of git: the parent of the common
    directory a real (but not necessarily `.claude/worktrees`-shaped) git
    worktree shares with it. None with no git, or outside a git checkout."""
    try:
        result = subprocess.run(
            ["git", "-C", str(here.parent), "rev-parse", "--path-format=absolute",
             "--git-common-dir", "--show-toplevel"],
            capture_output=True, text=True)
    except OSError:
        return None
    if result.returncode != 0:
        return None
    common, toplevel = result.stdout.splitlines()[:2]
    return Path(common).parent / here.relative_to(Path(toplevel).resolve())


def main_checkout_copy(tool):
    """The main checkout's file standing where `tool` stands in this one, or
    None when `tool` is that file or there is no main checkout to ask."""
    here = Path(tool).resolve()
    main = by_worktree_convention(here) or by_git_worktree(here)
    if main is None or not main.is_file() or main.resolve() == here:
        return None
    return main


def run_main_checkout_copy(tool, argv=None):
    """Hand this command line to the main checkout's copy of `tool` and exit
    with its status; return only when this copy should run itself."""
    if os.environ.get("AUTANA_DEVICE_TOOLS") == "here":
        return
    main = main_checkout_copy(tool)
    if main is None:
        return
    arguments = sys.argv[1:] if argv is None else argv
    sys.exit(subprocess.run([sys.executable, "-u", str(main), *arguments]).returncode)
