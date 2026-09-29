"""What a lock holder started, on POSIX, so nothing can keep the serial port
after the lock is gone.

Linux tags every process the holder starts through the environment: the lock's
token is exported as AUTANA_LOCK_TOKEN and inherited, so /proc names the
holder's descendants through any number of exited parents, which a process
tree walk cannot (an orphan is reparented to init). PR_SET_PDEATHSIG covers
only direct children and a subreaper dies with the holder, so neither can end
grandchildren of a killed holder. A small watchdog, started with the lock and
holding the read end of a pipe the holder keeps open, sees the holder end by
any means and kills every tagged process. A process that scrubs its own
environment is out of reach.

macOS has no /proc: the descendants of a living holder are found from the
process table and stopped on a normal exit; a killed holder's children are
not stopped there, and the exclusive port open makes that loud."""

import os
import signal
import subprocess
import sys
import time

TOKEN_VARIABLE = "AUTANA_LOCK_TOKEN"
GRACE_SECONDS = 2.0
POLL_SECONDS = 0.05
WATCHDOG_PATIENCE_SECONDS = 10.0

_stack = []


def report(text):
    print("device lock: " + text, file=sys.stderr)


def has_proc():
    return sys.platform.startswith("linux") and os.path.isdir("/proc/self")


def tagged(pid, token):
    """Whether `pid` was started under `token`. A zombie has no environment
    left to read, which is right: it holds nothing."""
    try:
        with open(f"/proc/{pid}/environ", "rb") as stream:
            data = stream.read()
    except OSError:
        return False
    return f"{TOKEN_VARIABLE}={token}".encode() in data.split(b"\0")


def tagged_pids(token, skip=()):
    return [int(name) for name in os.listdir("/proc")
            if name.isdigit() and int(name) not in skip and tagged(int(name), token)]


def descendants(root):
    """Live processes below `root`, from the process table. Only meaningful
    while `root` is alive: a dead parent's children belong to init."""
    output = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,stat="], capture_output=True,
                            text=True).stdout
    table = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0].isdigit() and fields[1].isdigit() \
                and not fields[2].startswith("Z"):
            table[int(fields[0])] = int(fields[1])
    found, frontier = [], [root]
    while frontier:
        parent = frontier.pop(0)
        for child, above in table.items():
            if above == parent and child not in found:
                found.append(child)
                frontier.append(child)
    return found


def stop(pid, token):
    """SIGKILL for `pid`. On Linux through a pidfd, after checking the process
    still carries the token: the number alone could by now be someone else's."""
    if not has_proc():
        try:
            os.kill(pid, signal.SIGKILL)
            return True
        except OSError:
            return False
    try:
        descriptor = os.pidfd_open(pid)
    except (AttributeError, OSError):
        descriptor = None
    try:
        if not tagged(pid, token):
            return False
        if descriptor is not None:
            signal.pidfd_send_signal(descriptor, signal.SIGKILL)
        else:
            os.kill(pid, signal.SIGKILL)
        return True
    except OSError:
        return False
    finally:
        if descriptor is not None:
            os.close(descriptor)


def enter(token):
    """Tags what this process starts from now on with `token`; on Linux also
    starts the watchdog. Always True: without the watchdog it says so."""
    watchdog = None
    if has_proc():
        try:
            watchdog = subprocess.Popen(
                [sys.executable, os.path.abspath(__file__), "watch", token],
                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True, env=dict(os.environ, **{TOKEN_VARIABLE: token}))
        except (OSError, ValueError) as error:
            report(f"could not start the watchdog ({error}); processes this command "
                   "starts are not stopped if it is killed")
    _stack.append((token, os.environ.get(TOKEN_VARIABLE), watchdog))
    os.environ[TOKEN_VARIABLE] = token
    return True


def leave():
    """Undoes the last enter(): the environment, and the watchdog, which
    finds nothing left to stop and ends."""
    if not _stack:
        return
    _, previous, watchdog = _stack.pop()
    if previous is None:
        os.environ.pop(TOKEN_VARIABLE, None)
    else:
        os.environ[TOKEN_VARIABLE] = previous
    if watchdog is not None:
        watchdog.stdin.close()
        try:
            watchdog.wait(WATCHDOG_PATIENCE_SECONDS)
        except subprocess.TimeoutExpired:
            watchdog.kill()
            watchdog.wait()


def members():
    """The pids started under the current lock, other than this process and
    its watchdog."""
    if not _stack:
        return []
    token, _, watchdog = _stack[-1]
    if has_proc():
        skip = {os.getpid()} | ({watchdog.pid} if watchdog else set())
        return tagged_pids(token, skip)
    return descendants(os.getpid())


def reap(before=frozenset(), grace=GRACE_SECONDS):
    """Gives the members that were not in `before` `grace` seconds to end, then
    kills the rest. Returns (killed, survivors)."""
    def started_here():
        return [pid for pid in members() if pid not in before]

    token = _stack[-1][0] if _stack else ""
    deadline = time.monotonic() + grace
    while started_here() and time.monotonic() < deadline:
        time.sleep(POLL_SECONDS)
    killed = [pid for pid in started_here() if stop(pid, token)]
    deadline = time.monotonic() + grace
    survivors = started_here()
    while survivors and time.monotonic() < deadline:
        time.sleep(POLL_SECONDS)
        survivors = started_here()
    return killed, survivors


def watch(token):
    """The watchdog: waits for the holder's end of the pipe to close, then
    kills every process still tagged with `token`."""
    sys.stdin.buffer.read()
    deadline = time.monotonic() + WATCHDOG_PATIENCE_SECONDS
    while time.monotonic() < deadline:
        left = tagged_pids(token, {os.getpid()})
        if not left:
            return
        for pid in left:
            stop(pid, token)
        time.sleep(POLL_SECONDS)


if __name__ == "__main__" and sys.argv[1:2] == ["watch"]:
    watch(sys.argv[2])
