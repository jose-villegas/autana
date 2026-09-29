"""A Windows job object that ends with the lock holder, so nothing the holder
started can keep the serial port after the lock is gone.

The holder joins a kill-on-close job; every process it starts inherits it,
grandchildren of dead parents included. When the holder dies by any means the
kernel closes the job and kills the members, so no pid is inferred or
trusted. Jobs nest since Windows 8, so a holder already inside a launcher's
or a harness's job still gets its own. A process that asks for
CREATE_BREAKAWAY_FROM_JOB may leave it. lock_group.py is the POSIX side."""

import os
import sys
import time

import device_lock

GRACE_SECONDS = 2.0
POLL_SECONDS = 0.05
KILL_ON_JOB_CLOSE = 0x2000
BREAKAWAY_OK = 0x800
EXTENDED_LIMIT_INFORMATION = 9
BASIC_PROCESS_ID_LIST = 3
MAX_MEMBERS = 256
ERROR_MORE_DATA = 234
PROCESS_TERMINATE = 0x0001
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

_job = None
_kernel32 = None
_reported = set()


def report(text):
    """A message on stderr, once per process: a poll loop must not repeat it."""
    if text not in _reported:
        _reported.add(text)
        print("device lock: " + text, file=sys.stderr)


def binding():
    """kernel32 with the job calls typed, built on first use."""
    global _kernel32
    if _kernel32 is None:
        import ctypes
        from ctypes import wintypes
        kernel32 = device_lock.windows_kernel32()
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int,
                                                     ctypes.c_void_p, wintypes.DWORD]
        kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel32.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int,
                                                       ctypes.c_void_p, wintypes.DWORD,
                                                       ctypes.c_void_p]
        kernel32.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE,
                                            ctypes.POINTER(wintypes.BOOL)]
        kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        _kernel32 = kernel32
    return _kernel32


def create_job(kernel32):
    """A job that kills its members when its last handle closes, or None."""
    import ctypes
    from ctypes import wintypes

    class BasicLimits(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                    ("PerJobUserTimeLimit", ctypes.c_int64), ("LimitFlags", wintypes.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t),
                    ("ActiveProcessLimit", wintypes.DWORD), ("Affinity", ctypes.c_size_t),
                    ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("Basic", BasicLimits), ("IoCounters", ctypes.c_uint64 * 6),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        return None
    limits = ExtendedLimits()
    limits.Basic.LimitFlags = KILL_ON_JOB_CLOSE | BREAKAWAY_OK
    if kernel32.SetInformationJobObject(job, EXTENDED_LIMIT_INFORMATION,
                                        ctypes.byref(limits), ctypes.sizeof(limits)):
        return job
    kernel32.CloseHandle(job)
    return None


def enter(token=None):
    """Puts this process in a job of its own, once. False, with the reason on
    stderr, where that is not possible: the lock then works as it always did."""
    global _job
    if os.name != "nt":
        return False
    if _job is not None:
        return True
    kernel32 = binding()
    job = create_job(kernel32)
    if job and kernel32.AssignProcessToJobObject(job, kernel32.GetCurrentProcess()):
        _job = job
        return True
    if job:
        kernel32.CloseHandle(job)
    report("could not join a job object; processes this command starts are not "
           "stopped with it")
    return False


def leave():
    """The job lasts as long as the process."""


def members():
    """The pids in this process's job other than this one. A list that cannot
    be read in full is said so, never passed off as the whole."""
    if _job is None:
        return []
    import ctypes
    from ctypes import wintypes

    class ProcessIdList(ctypes.Structure):
        _fields_ = [("Assigned", wintypes.DWORD), ("Listed", wintypes.DWORD),
                    ("Ids", ctypes.c_size_t * MAX_MEMBERS)]

    found = ProcessIdList()
    listed = binding().QueryInformationJobObject(_job, BASIC_PROCESS_ID_LIST,
                                                 ctypes.byref(found), ctypes.sizeof(found), None)
    if not listed and ctypes.get_last_error() != ERROR_MORE_DATA:
        report("could not list the job's processes (error " + str(ctypes.get_last_error())
               + "); nothing started under the lock can be stopped")
        return []
    if not listed or found.Assigned > found.Listed:
        report(f"the job has {found.Assigned} processes and only {found.Listed} can be listed; "
               "the rest are not stopped when the lock is released")
    return [pid for pid in found.Ids[:found.Listed] if pid != os.getpid()]


def survivors(record):
    """A finished holder's job died with it, so what is left is the holder
    itself if it still runs, or a process that broke away; only the holder's
    pid is known."""
    pid = record.get("pid")
    if not isinstance(pid, int) or pid == os.getpid() or not device_lock.process_alive(pid):
        return []
    return [(pid, "")]


def terminate_member(pid):
    """Stops `pid` through a handle, and only if it is in this job: the pid
    alone could by now belong to an unrelated process."""
    import ctypes
    from ctypes import wintypes

    kernel32 = binding()
    handle = kernel32.OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                                  False, pid)
    if not handle:
        return False
    try:
        inside = wintypes.BOOL()
        if not kernel32.IsProcessInJob(handle, _job, ctypes.byref(inside)) or not inside.value:
            return False
        return bool(kernel32.TerminateProcess(handle, 1))
    finally:
        kernel32.CloseHandle(handle)


def reap(before=frozenset(), grace=GRACE_SECONDS):
    """Gives the members that were not in `before` `grace` seconds to end, then
    stops the rest. `before` is what the job held when the lock was taken:
    that work is not this lock's. Returns (killed, survivors), the pids that
    were stopped and the ones still running afterwards."""
    def started_here():
        return [pid for pid in members() if pid not in before]

    deadline = time.monotonic() + grace
    while started_here() and time.monotonic() < deadline:
        time.sleep(POLL_SECONDS)
    killed = [pid for pid in started_here() if terminate_member(pid)]
    deadline = time.monotonic() + grace
    survivors = started_here()
    while survivors and time.monotonic() < deadline:
        time.sleep(POLL_SECONDS)
        survivors = started_here()
    return killed, survivors
