"""Stop foreground commands together with children that hold pipes or locks."""
import os
import signal
import subprocess


def launch_process_tree(command, **options):
    """Assign Windows children before they can spawn outside the job."""
    if os.name != "nt":
        options["start_new_session"] = True
        return subprocess.Popen(command, **options)
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

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    job = kernel.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    limits = ExtendedLimits()
    limits.basic.flags = 0x2000  # Descendants must not outlive capture ownership.
    process = None
    try:
        if not kernel.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            raise ctypes.WinError(ctypes.get_last_error())
        options["creationflags"] = options.get("creationflags", 0) | 0x4
        process = subprocess.Popen(command, **options)
        if not kernel.AssignProcessToJobObject(job, int(process._handle)):
            raise ctypes.WinError(ctypes.get_last_error())
        resume = ctypes.WinDLL("ntdll").NtResumeProcess
        resume.argtypes = [wintypes.HANDLE]
        if resume(int(process._handle)) < 0:
            raise OSError("cannot resume capture process")
        process._tree_job = job
        return process
    except BaseException:
        if process is not None:
            process.kill()
            process.wait()
        kernel.CloseHandle(job)
        raise


def close_process_tree(process):
    """Release job ownership, including any remaining Windows descendants."""
    job = getattr(process, "_tree_job", None)
    if os.name == "nt" and job:
        import ctypes
        from ctypes import wintypes
        close = ctypes.WinDLL("kernel32").CloseHandle
        close.argtypes = [wintypes.HANDLE]
        close(job)
        process._tree_job = None


def terminate_tree(process):
    """Escalate POSIX termination; closing a Windows job kills its tree."""
    if os.name == "nt":
        close_process_tree(process)
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
    """Stop a process tree; POSIX callers must start a new session."""
    terminate_tree(process)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass
    finally:
        close_process_tree(process)
