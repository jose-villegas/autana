"""Run the main checkout's copy of a board tool instead of this one.

Every worktree carries its own scripts/device/, but the lock they share is one
set of files, so two versions of the lock code on one machine can each believe
they hold the board. A board tool started from a worktree therefore hands its
command line to the same-named tool in the main checkout (the parent of git's
common directory) and exits with its status. AUTANA_DEVICE_TOOLS=here runs
this copy instead, for working on the tools themselves.
"""

import os
import subprocess
import sys
from pathlib import Path


def main_checkout_copy(tool):
    """The main checkout's file standing where `tool` stands in this one, or
    None when `tool` is that file or there is no main checkout to ask."""
    here = Path(tool).resolve()
    result = subprocess.run(
        ["git", "-C", str(here.parent), "rev-parse", "--path-format=absolute",
         "--git-common-dir", "--show-toplevel"],
        capture_output=True, text=True)
    if result.returncode != 0:
        return None
    common, toplevel = result.stdout.splitlines()[:2]
    main = Path(common).parent / here.relative_to(Path(toplevel).resolve())
    if not main.is_file() or main.resolve() == here:
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
