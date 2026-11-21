"""One run at a time in a build directory: its lock file, and a command run
while holding it.

    python launcher/tools/build/build_lock.py DIR COMMAND [ARG...]

Runs COMMAND once no other holder has DIR's lock, waiting as long as that
takes, and exits with COMMAND's status. The operating system drops the lock
when its holder exits, however it exits, so a killed run leaves none behind.
"""
import os
from pathlib import Path
import subprocess
import sys
import time

# The lock file in a build directory; build_paths.py keeps it across a reset.
BUILD_LOCK = 'autana-build.lock'
POLL_SECONDS = 0.2


def try_lock_file(handle):
    """Takes the lock on an open file without waiting: True when taken."""
    try:
        if os.name == 'nt':
            import msvcrt
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def waiting_line(build_dir):
    """What a run prints while another holds `build_dir`."""
    return f'waiting for another run in {build_dir}'


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    build_dir = Path(argv[1])
    build_dir.mkdir(parents=True, exist_ok=True)
    with open(build_dir / BUILD_LOCK, 'a+b') as handle:
        if not try_lock_file(handle):
            print(waiting_line(build_dir), file=sys.stderr, flush=True)
            while not try_lock_file(handle):
                time.sleep(POLL_SECONDS)
        return subprocess.run(argv[2:]).returncode


if __name__ == '__main__':
    sys.exit(main(sys.argv))
