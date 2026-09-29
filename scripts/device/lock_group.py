"""What a lock holder started, on Linux, so nothing can keep the serial port
after the lock is gone.

Every process the holder starts is tagged through the environment: the lock's
token is exported as AUTANA_DEVICE_LOCK_TOKEN and inherited, so /proc names
the holder's descendants through any number of exited parents, which a process
tree walk cannot (an orphan is reparented to init). PR_SET_PDEATHSIG covers
only direct children and a subreaper dies with the holder, so neither can end
grandchildren of a killed holder. A small watchdog, started with the lock and
holding the read end of a pipe the holder keeps open, sees the holder end by
any means and kills every tagged process. A process that scrubs its own
environment or runs as another user is out of reach. Without /proc this does
nothing."""

import os
import signal
import subprocess
import sys
import time

TOKEN_VARIABLE = "AUTANA_DEVICE_LOCK_TOKEN"
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


def stop(pid, token):
    """SIGKILL for `pid` through a pidfd, after checking the process still
    carries the token: the number alone could by now be someone else's."""
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


def survivors_extra(record):
    """Pids still carrying a finished holder's token. The holder's own process
    is not among them - it set the token after it started - so the caller
    checks the record's pid itself."""
    token = record.get("token")
    if not token or not has_proc():
        return []
    return tagged_pids(token, {os.getpid()})


def process_name(pid):
    try:
        with open(f"/proc/{pid}/comm", encoding="utf-8", errors="replace") as stream:
            return stream.read().strip()
    except OSError:
        return ""


def process_start(pid):
    """When `pid` began, in epoch seconds, or None where /proc cannot say."""
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8", errors="replace") as stream:
            stat = stream.read()
        with open("/proc/stat", encoding="utf-8") as stream:
            boot = next(int(line.split()[1]) for line in stream if line.startswith("btime"))
        # Field 22, counted from after the parenthesised command name.
        ticks = int(stat[stat.rindex(")") + 2:].split()[19])
        return boot + ticks / os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError, StopIteration):
        return None


def enter(token):
    """Tags what this process starts from now on with `token` and starts the
    watchdog, which is itself untagged. False where there is no /proc."""
    if not has_proc():
        return False
    clean = {name: value for name, value in os.environ.items() if name != TOKEN_VARIABLE}
    watchdog = None
    try:
        watchdog = subprocess.Popen(
            [sys.executable, os.path.abspath(__file__), "watch", token],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True, env=clean)
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
    """The pids started under the current lock, other than this process."""
    if not _stack:
        return []
    return tagged_pids(_stack[-1][0], {os.getpid()})


def reap(before=frozenset(), grace=GRACE_SECONDS):
    """Gives the members that were not in `before` `grace` seconds to end, then
    kills the rest. Returns (killed, survivors)."""
    def started_here():
        return [pid for pid in members() if pid not in before]

    if not _stack:
        return [], []
    token, _, watchdog = _stack[-1]
    if watchdog is None or watchdog.poll() is not None:
        report("the watchdog is not running; a killed holder would have left its "
               "processes behind")
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
