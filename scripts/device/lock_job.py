"""A Windows job object that ends with the lock holder, so nothing the holder
started can keep the serial port after the lock is gone.

The holder joins a kill-on-close job; every process it starts inherits it,
grandchildren of dead parents included. When the holder dies by any means the
kernel closes the job and kills the members, so no pid is inferred or
trusted. Jobs nest since Windows 8, so a holder already inside a launcher's
or a harness's job still gets its own. POSIX has no equivalent: there a
killed holder's children are not stopped."""

import os
import signal
import sys
import time

import device_lock

GRACE_SECONDS = 2.0
POLL_SECONDS = 0.05
KILL_ON_JOB_CLOSE = 0x2000
EXTENDED_LIMIT_INFORMATION = 9
BASIC_PROCESS_ID_LIST = 3
MAX_MEMBERS = 256

_job = None


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

    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                 wintypes.DWORD]
    job = kernel32.CreateJobObjectW(None, None)
    limits = ExtendedLimits()
    limits.Basic.LimitFlags = KILL_ON_JOB_CLOSE
    if job and kernel32.SetInformationJobObject(job, EXTENDED_LIMIT_INFORMATION,
                                                ctypes.byref(limits), ctypes.sizeof(limits)):
        return job
    return None


def enter():
    """Puts this process in a job of its own, once. False, with the reason on
    stderr, where that is not possible: the lock then works as it always did."""
    global _job
    if os.name != "nt":
        return False
    if _job is not None:
        return True
    from ctypes import wintypes
    kernel32 = device_lock.windows_kernel32()
    kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    job = create_job(kernel32)
    if not job or not kernel32.AssignProcessToJobObject(job, kernel32.GetCurrentProcess()):
        print("device lock: could not join a job object; processes this command starts "
              "are not stopped with it", file=sys.stderr)
        return False
    _job = job
    return True


def members():
    """The pids in this process's job other than this one."""
    if _job is None:
        return []
    import ctypes
    from ctypes import wintypes

    class ProcessIdList(ctypes.Structure):
        _fields_ = [("Assigned", wintypes.DWORD), ("Listed", wintypes.DWORD),
                    ("Ids", ctypes.c_size_t * MAX_MEMBERS)]

    kernel32 = device_lock.windows_kernel32()
    kernel32.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                   wintypes.DWORD, ctypes.c_void_p]
    found = ProcessIdList()
    if not kernel32.QueryInformationJobObject(_job, BASIC_PROCESS_ID_LIST, ctypes.byref(found),
                                              ctypes.sizeof(found), None):
        return []
    return [pid for pid in found.Ids[:found.Listed] if pid != os.getpid()]


def reap(grace=GRACE_SECONDS):
    """Gives the members `grace` seconds to end, then kills the rest. Returns
    the pids that had to be killed."""
    deadline = time.monotonic() + grace
    while members() and time.monotonic() < deadline:
        time.sleep(POLL_SECONDS)
    left = members()
    for pid in left:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    while members() and time.monotonic() < deadline + grace:
        time.sleep(POLL_SECONDS)
    return left
