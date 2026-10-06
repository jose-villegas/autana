#!/usr/bin/env python3
"""Fail on a module of the firmware or its suites with no header comment.

    python scripts/gates/check_file_headers.py

A module is a .c and its .h with the same name in the same folder; one of
them opens with a comment that says what the module is (a `#pragma once`
may come first). The rest of a header's rules are check_comment_length.py's,
which also owns what counts as a header.

Scope is by folder, not by list: every tracked .c and .h under launcher/main/
and launcher/test/suites/. A suite (suite_*.c) is exempt: its test names say
what it proves. So are the sources check_comment_length.py excludes (vendored
and generated). The editor, the test harness and its stubs are outside the
scope for now.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from c_comments import file_header, sources  # noqa: E402

SCOPE = ("launcher/main/", "launcher/test/suites/")


def in_scope(rp):
    name = rp.rsplit("/", 1)[-1]
    return (rp.startswith(SCOPE) and not name.startswith("suite_"))


def problems(root="."):
    modules = {}
    for path in sources(root, tracked=True):
        rp = path.relative_to(root).as_posix()
        if in_scope(rp):
            modules.setdefault(rp.rsplit(".", 1)[0], []).append(rp)
    found = []
    for stem, files in sorted(modules.items()):
        texts = {rp: (pathlib.Path(root) / rp).read_text(encoding="utf-8", errors="replace") for rp in files}
        if not any(file_header(rp, text) for rp, text in texts.items()):
            found.append(f"{sorted(files)[-1]}: no header comment - open it with one saying what the module is")
    return found


def main():
    found = problems()
    for line in found:
        print(line)
    print(f"{len(found)} module(s) without a header comment")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
