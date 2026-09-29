"""The processes a process started, so the device lock can outlive them.

The lock arbitrates intent while the OS owns the serial port, and a child
(esptool, a monitor, a reader) can keep the port after its parent has moved
on. Both platforms answer "who are my descendants" from the process table:
Windows keeps a dead parent's pid on its children, POSIX gives it to init, so
this is only meaningful while the parent is alive - which is when the holder
records and reaps them."""

import os
import signal
import subprocess
import time

REAP_GRACE_SECONDS = 2.0
REAP_KILL_SECONDS = 3.0
POLL_SECONDS = 0.05


def parent_table():
    """{pid: parent pid} for every running process; a zombie on POSIX is over."""
    if os.name == "nt":
        return windows_parent_table()
    output = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,stat="], capture_output=True,
                            text=True).stdout
    table = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0].isdigit() and fields[1].isdigit()                 and not fields[2].startswith("Z"):
            table[int(fields[0])] = int(fields[1])
    return table


def windows_parent_table():
    import ctypes
    from ctypes import wintypes

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
                    ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if snapshot in (None, wintypes.HANDLE(-1).value):
        return {}
    table = {}
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(ProcessEntry)
        found = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while found:
            table[entry.th32ProcessID] = entry.th32ParentProcessID
            found = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return table


def descendants(pid=None, table=None):
    """Every process below `pid` (this one by default), children first found
    first. An empty answer when the table cannot be read."""
    root = os.getpid() if pid is None else pid
    table = parent_table() if table is None else table
    found = []
    frontier = [root]
    while frontier:
        parent = frontier.pop(0)
        for child, above in table.items():
            if above == parent and child != root and child not in found:
                found.append(child)
                frontier.append(child)
    return found


def running(alive, pid):
    """`alive(pid)`, after collecting the exit of a child of ours that is only
    a zombie: kill(0) still succeeds on one until its parent waits."""
    if os.name != "nt":
        try:
            os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            pass
    return alive(pid)


def kill(pid):
    try:
        os.kill(pid, signal.SIGTERM if os.name == "nt" else signal.SIGKILL)
    except OSError:
        pass


def reap(alive, pid=None, grace=REAP_GRACE_SECONDS, patience=REAP_KILL_SECONDS,
         sleep=time.sleep, clock=time.monotonic):
    """Waits `grace` for this process's descendants to end by themselves, then
    kills what is left. Returns (killed, survivors): the pids that had to be
    killed and the ones still running `patience` later. `alive` is the
    liveness test, so the caller's own is honoured."""
    remaining = descendants(pid)
    deadline = clock() + grace
    while remaining and clock() < deadline:
        sleep(POLL_SECONDS)
        remaining = [child for child in remaining if running(alive, child)]
    killed = list(remaining)
    for child in killed:
        kill(child)
    deadline = clock() + patience
    while remaining and clock() < deadline:
        sleep(POLL_SECONDS)
        remaining = [child for child in remaining if running(alive, child)]
    return killed, remaining
