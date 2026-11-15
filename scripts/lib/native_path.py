"""Native drive paths for tools launched from Git Bash, and Git Bash itself; Linux paths pass through."""
import os
import re
import shutil
from pathlib import Path


def to_native(path):
    match = re.match(r'^/([A-Za-z])/(.*)$', path)
    if match and os.name == 'nt':
        return f'{match.group(1)}:/{match.group(2)}'
    return path


def git_bash():
    """The bash to run a script with. On Windows a bare `bash` is System32's WSL launcher (process creation
    searches System32 before PATH), which hands the script to Linux bash and cannot run ESP-IDF."""
    if os.name != "nt":
        return "bash"
    candidates = []
    git = shutil.which("git")
    if git:
        git_root = Path(git).resolve().parent.parent
        candidates += [git_root / "bin" / "bash.exe", git_root / "usr" / "bin" / "bash.exe"]
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432")):
        if base:
            candidates.append(Path(base) / "Git" / "bin" / "bash.exe")
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("Git Bash not found; install Git for Windows")
