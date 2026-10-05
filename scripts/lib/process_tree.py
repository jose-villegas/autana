"""Start and stop commands as one process tree, so children holding pipes or locks die with them."""
import os
import signal
import subprocess
import sys

JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
JOB_OBJECT_LIMIT_BREAKAWAY_OK = 0x800
JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
CREATE_SUSPENDED = 0x4


def windows_job_binding(kernel=None):
    import ctypes
    from ctypes import wintypes
    kernel = kernel or ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    return kernel


def create_kill_on_close_job(breakaway_ok=False, kernel=None):
    """Create a Windows job, or return None when ownership is unavailable."""
    import ctypes
    from ctypes import wintypes
    class BasicLimits(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                    ("flags", wintypes.DWORD), ("minimum", ctypes.c_size_t),
                    ("maximum", ctypes.c_size_t), ("active", wintypes.DWORD),
                    ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD),
                    ("scheduling", wintypes.DWORD)]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("basic", BasicLimits), ("io", ctypes.c_uint64 * 6),
                    ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                    ("peak_process", ctypes.c_size_t), ("peak_job", ctypes.c_size_t)]

    kernel = windows_job_binding(kernel)
    job = kernel.CreateJobObjectW(None, None)
    if job:
        limits = ExtendedLimits()
        limits.basic.flags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if breakaway_ok:
            limits.basic.flags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK
        if kernel.SetInformationJobObject(job, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                                          ctypes.byref(limits), ctypes.sizeof(limits)):
            return job
        kernel.CloseHandle(job)
    return None


def launch_process_tree(command, **options):
    """Start command as one stoppable tree: a new session on POSIX, a job assigned before it runs on Windows."""
    if os.name != "nt":
        options["start_new_session"] = True
        return subprocess.Popen(command, **options)
    import ctypes
    from ctypes import wintypes
    kernel = windows_job_binding()
    job = create_kill_on_close_job(kernel=kernel)
    if not job:
        print("process tree: could not create a job object; using tree kill", file=sys.stderr)
        return subprocess.Popen(command, **options)
    process = None
    try:
        options["creationflags"] = options.get("creationflags", 0) | CREATE_SUSPENDED
        process = subprocess.Popen(command, **options)
        if kernel.AssignProcessToJobObject(job, int(process._handle)):
            process._tree_job = job
        else:
            kernel.CloseHandle(job)
            job = None
            print("process tree: could not assign a job object; using tree kill", file=sys.stderr)
        # Popen closes the thread handle, so _handle and NtResumeProcess are used instead of ResumeThread.
        resume = ctypes.WinDLL("ntdll").NtResumeProcess
        resume.argtypes = [wintypes.HANDLE]
        if resume(int(process._handle)) < 0:
            raise OSError("cannot resume capture process")
        return process
    except BaseException:
        if process is not None:
            process.kill()
            process.wait()
        if job:
            kernel.CloseHandle(job)
            if process is not None:
                process._tree_job = None
        raise


def close_process_tree(process):
    """Close the Windows job, killing any descendant still in it; a no-op on POSIX."""
    job = getattr(process, "_tree_job", None)
    if os.name == "nt" and job:
        import ctypes
        from ctypes import wintypes
        close = ctypes.WinDLL("kernel32").CloseHandle
        close.argtypes = [wintypes.HANDLE]
        close(job)
        process._tree_job = None


def terminate_tree(process):
    """Escalate POSIX termination; on Windows close the job, or taskkill the tree when no job was assigned."""
    if os.name == "nt":
        if getattr(process, "_tree_job", None):
            close_process_tree(process)
        else:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def stop_process_tree(process):
    """Stop a tree started by launch_process_tree, then release its job."""
    terminate_tree(process)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass
    finally:
        close_process_tree(process)
