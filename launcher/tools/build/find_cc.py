"""The host C compiler find_cc.sh picks, for Python tools: one owner of that choice."""
import pathlib
import shutil
import subprocess

FIND_CC = pathlib.Path(__file__).with_name("find_cc.sh")


def host_cc():
    """The compiler's resolved path, or None when there is none (or no sh to ask).
    Resolving through PATH turns a Git Bash spelling (/c/...) into one Windows runs."""
    try:
        found = subprocess.run(["sh", "-c", f'. "{FIND_CC.as_posix()}" && find_cc'], stdin=subprocess.DEVNULL,
                               capture_output=True, text=True)
    except OSError:
        return None
    cc = found.stdout.strip()
    if found.returncode != 0 or not cc:
        return None
    return shutil.which(cc) or cc
