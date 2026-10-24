#!/usr/bin/env python3
"""Fail when autana opens a firmware source file while its tests run.

    python scripts/gates/check_autana_source.py [ROOT]

autana is the device tool: it asks the board, and never reads or writes
launcher/main or launcher/packages (docs/cli/README.md). This runs every test
under ROOT/scripts/autana/tests with an audit hook on `open`; an open of a
path under either folder fails the gate unless the nearest frame in ROOT that
made it is a test file, since a test may compare a reply with the source. The
hook sees every open, so code autana imports (scripts/lib/...) is held to the
same rule. The docs tooling that may read source, shared_helpers.py, must not
be imported at all.
"""
import os
import re
import sys
import unittest
from pathlib import Path

SOURCE_PATH = re.compile(r"(^|/)launcher/(main|packages)(/|$)")
DOCS_TOOLING = "shared_helpers"


def folder(path):
    return os.path.normcase(os.path.abspath(path)) + os.sep


def opener(frame, root, tests):
    """The file under root that asked for this open, or None when a test did."""
    while frame is not None:
        name = os.path.normcase(os.path.abspath(frame.f_code.co_filename))
        if name.startswith(root):
            return None if name.startswith(tests) else os.path.abspath(frame.f_code.co_filename)
        frame = frame.f_back
    return None


def run(root):
    """Runs the tests under the hook; returns (test result, [(opener, path)])."""
    tests = Path(root) / "scripts" / "autana" / "tests"
    root_folder, tests_folder = folder(root), folder(tests)
    found = []

    def hook(event, args):
        if event != "open" or isinstance(args[0], int):
            return
        path = os.fsdecode(args[0]).replace("\\", "/")
        if SOURCE_PATH.search(path):
            by = opener(sys._getframe(1), root_folder, tests_folder)
            if by is not None:
                found.append((by, path))

    suite = unittest.defaultTestLoader.discover(str(tests), top_level_dir=str(tests))
    sys.addaudithook(hook)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return result, found


def main(argv):
    root = Path(argv[0] if argv else Path(__file__).resolve().parents[2]).resolve()
    result, found = run(root)
    for by, path in sorted(set(found)):
        print(f"{Path(by).relative_to(root).as_posix()}: opened {path} - autana asks the device, "
              f"source belongs to source tools", file=sys.stderr)
    imported = DOCS_TOOLING in sys.modules
    if imported:
        print(f"{DOCS_TOOLING} was imported - it is docs tooling, not autana's", file=sys.stderr)
    return 0 if result.wasSuccessful() and not found and not imported else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
