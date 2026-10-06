"""Native drive paths for tools launched from Git Bash; Linux paths pass through."""
import os
import re
import subprocess
from shutil import which


def to_native(path, windows=False):
    if windows and which("cygpath"):
        return subprocess.check_output(["cygpath", "-w", path], text=True).rstrip("\n")
    match = re.match(r'^/([A-Za-z])/(.*)$', path)
    if match and os.name == 'nt':
        return f'{match.group(1)}:/{match.group(2)}'
    return path
