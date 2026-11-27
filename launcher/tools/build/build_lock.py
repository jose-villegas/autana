"""One run at a time in a build directory: its lock file, and a command run
while holding it.

    python launcher/tools/build/build_lock.py DIR COMMAND [ARG...]

Runs COMMAND once no other holder has DIR's lock, waiting as long as that
takes, and exits with COMMAND's status. The lock belongs to the command's
process tree (on POSIX the command replaces this process and keeps the lock's
file open), and the operating system drops it when the holder exits, however
it exits, so a killed run leaves none behind. A waiting run names the holder's
process id, so a run left behind can be found.
"""
import contextlib
import os
from pathlib import Path
import subprocess
import sys
import time

USAGE = 'usage: build_lock.py DIR COMMAND [ARG...]'
# The lock file in a build directory; build_paths.py keeps it across a reset.
BUILD_LOCK = 'autana-build.lock'
POLL_SECONDS = 0.2
# Windows locks this byte of the file; the holder's id follows it, readable
# while the byte is locked.
LOCKED_BYTES = 1
# The status a shell gives a run stopped by Ctrl-C (128 + SIGINT).
INTERRUPTED = 130


def try_lock_file(handle):
    """Takes the lock on an open file without waiting: True when taken."""
    try:
        if os.name == 'nt':
            import msvcrt
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, LOCKED_BYTES)
        else:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def waiting_line(build_dir, holder=None):
    """What a run prints while another, process `holder` if known, holds `build_dir`."""
    return f'waiting for another run in {build_dir}' + (f' (held by pid {holder})' if holder else '')


def holder_pid(handle):
    """The process id the holder wrote past the locked byte, or None."""
    try:
        handle.seek(LOCKED_BYTES)
        text = handle.read().decode('ascii', 'replace').strip()
    except OSError:
        return None
    return text if text.isdigit() else None


@contextlib.contextmanager
def hold(build_dir):
    """Holds `build_dir`'s lock for the block, waiting for any other holder;
    yields the open lock file."""
    build_dir = Path(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)
    path = build_dir / BUILD_LOCK
    with open(os.open(path, os.O_RDWR | os.O_CREAT), 'r+b') as handle:
        if not try_lock_file(handle):
            print(waiting_line(build_dir, holder_pid(handle)), file=sys.stderr, flush=True)
            while not try_lock_file(handle):
                time.sleep(POLL_SECONDS)
        handle.seek(0)
        handle.truncate()
        handle.write(b'#' * LOCKED_BYTES + f'{os.getpid()}\n'.encode('ascii'))
        handle.flush()
        yield handle


def main(argv):
    if len(argv) < 3:
        print(USAGE, file=sys.stderr)
        return 2
    command = argv[2:]
    with hold(argv[1]) as handle:
        if os.name != 'nt':
            # The command keeps the lock's file, so the lock lasts as long as it does.
            os.set_inheritable(handle.fileno(), True)
            os.execvp(command[0], command)
        # A Windows lock belongs to its process, so this one stays until the command ends.
        try:
            return subprocess.run(command).returncode
        except KeyboardInterrupt:
            return INTERRUPTED


if __name__ == '__main__':
    sys.exit(main(sys.argv))
