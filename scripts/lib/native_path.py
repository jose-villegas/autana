"""Native drive paths for tools launched from Git Bash; Linux paths pass through."""
import os
import re
import subprocess
import sys
from shutil import which


def to_native(path, windows=False):
    if windows and which("cygpath"):
        return subprocess.check_output(["cygpath", "-w", path], text=True).rstrip("\n")
    match = re.match(r'^/([A-Za-z])/(.*)$', path)
    if match and os.name == 'nt':
        return f'{match.group(1)}:/{match.group(2)}'
    return path


if __name__ == "__main__":
    args = sys.argv[1:]
    absolute_only = "--absolute-only" in args[:1]
    if absolute_only:
        args.pop(0)
    if args:
        path = args[0]
        print(to_native(path, windows=not absolute_only or path.startswith("/")))
    elif which("cygpath"):
        sys.exit(subprocess.call(["cygpath", "-m", "-f", "-"]))
    else:
        sys.stdout.write(sys.stdin.read())
