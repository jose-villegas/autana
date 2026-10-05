"""Process admission against available host, cgroup and device memory."""
import concurrent.futures
import functools
import multiprocessing
import os
import pathlib
import subprocess
import time
import threading
import traceback
import signal
import sys
import ctypes
from collections import deque

GIB = 1024 ** 3
WSL_MEMORY_REQUIRED_BYTES = 6 * GIB
WINDOWS_MEMORY_REQUIRED_BYTES = 2 * GIB
# Estimates are (WSL RSS, GPU reserved, cgroup bytes), re-derived from workers' logged peaks.
# Prepare includes its pose pool.
FLOORS = (GIB, GIB // 2, GIB // 4)
FIT_BYTES = (22 * GIB // 10, 7 * GIB // 10, 22 * GIB // 10)
PREPARE_BYTES = (13 * GIB // 2, 0, 13 * GIB // 2)
BAKE_BYTES = (5 * GIB // 2, 0, 5 * GIB // 2)
MEASURE_BYTES = (3 * GIB // 2, 128 * 1024 ** 2, 3 * GIB // 2)
# Ray-query pose workers share almost nothing (about 1.9 GB each, measured),
# so this explicit pool budget and live free memory at fork time cap their count.
POSE_POOL_BYTES = 7 * GIB
SMOKE_PREPARE_BYTES = (2 * GIB, 0, 2 * GIB)
GPU_QUERY_FAILURE_SECONDS = 300
GPU_QUERY_LOG_SECONDS = 60
_task_reservation = None


def task_reservation():
    return _task_reservation


def meminfo_bytes():
    return {line.split(':')[0]: int(line.split()[1]) * 1024
            for line in pathlib.Path('/proc/meminfo').read_text().splitlines()
            if line.startswith('MemAvailable:')}


def worker_capacity(available, estimates, floors, cores):
    """Number of additional workers whose projected allocation preserves every floor."""
    if not len(available) == len(estimates) == len(floors) or cores < 0:
        raise ValueError("invalid resource dimensions or core count")
    count = cores
    for free, estimate, floor in zip(available, estimates, floors):
        if estimate < 0 or floor < 0:
            raise ValueError("negative estimate or floor")
        if free < floor:
            return 0
        if estimate:
            count = min(count, max(0, (free - floor) // estimate))
    return int(count)


def available_bytes(gpu=False):
    unlimited = 1 << 60
    wsl = meminfo_bytes()['MemAvailable']
    device = unlimited
    if gpu:
        rows = subprocess.check_output(["nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"], timeout=10).decode()
        device = int(rows.splitlines()[0]) * 1024 ** 2
    cgroup = unlimited
    for line in pathlib.Path("/proc/self/cgroup").read_text().splitlines():
        if line.startswith("0::"):
            directory = pathlib.Path("/sys/fs/cgroup") / line[3:].lstrip("/")
            while directory != pathlib.Path("/sys/fs"):
                maximum = directory / "memory.max"
                if maximum.exists() and maximum.read_text().strip() != "max":
                    cgroup = min(cgroup, int(maximum.read_text()) - int((directory / "memory.current").read_text()))
                directory = directory.parent
    return wsl, device, cgroup


def cores_available():
    return len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else os.cpu_count() or 1


def projected_available(available, workers):
    """Subtract each worker's not-yet-resident estimate."""
    return tuple(free - sum(max(0, estimate[index] - resident[index]) for estimate, resident in workers)
                 for index, free in enumerate(available))


def status_bytes(pid, field):
    try:
        return next(int(line.split()[1]) * 1024
                    for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines()
                    if line.startswith(field + ":"))
    except (OSError, StopIteration):
        return 0


def pss_bytes(pid):
    """Proportional resident bytes; zero when procfs PSS is unavailable."""
    try:
        return next(int(line.split()[1]) * 1024
                    for line in pathlib.Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()
                    if line.startswith("Pss:"))
    except (OSError, StopIteration):
        return 0


def resident_bytes(pid, gpu):
    rss = status_bytes(pid, "VmRSS")
    return rss, gpu.get(pid, 0), rss


def gpu_resident_bytes():
    gpu = {}
    if pathlib.Path("/proc").exists():
        rows = subprocess.check_output(["nvidia-smi", "--query-compute-apps=pid,used_memory",
                                        "--format=csv,noheader,nounits"], timeout=10).decode()
        for row in rows.splitlines():
            fields = row.split(",")
            if len(fields) == 2 and fields[1].strip().isdigit():
                gpu[int(fields[0])] = int(fields[1]) * 1024 ** 2
    return gpu


def parent_death_signal(parent_pid):
    if sys.platform == "linux":
        library = ctypes.CDLL(None, use_errno=True)
        if library.prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
            raise OSError(ctypes.get_errno(), "PR_SET_PDEATHSIG failed")
        if os.getppid() != parent_pid:
            os.kill(os.getpid(), signal.SIGKILL)


def peak_rss(pid=None):
    return status_bytes(pid or os.getpid(), "VmHWM")


def function_name(function):
    while isinstance(function, functools.partial):
        function = function.func
    return getattr(function, '__name__', type(function).__name__)


def _task(connection, function, args, estimates=FIT_BYTES, parent_pid=None):
    global _task_reservation
    parent_death_signal(parent_pid if parent_pid is not None else os.getppid())
    if hasattr(os, "setsid"):
        os.setsid()
    _task_reservation = estimates
    started = time.monotonic()
    try:
        connection.send((True, function(*args)))
    except BaseException:
        detail = traceback.format_exc()
        print(detail, flush=True)
        connection.send((False, detail))
    finally:
        torch = sys.modules.get("torch")
        device = torch.cuda.max_memory_reserved() if torch and torch.cuda.is_initialized() else 0
        name = function_name(function) + ":" + ",".join(str(arg) for arg in args if isinstance(arg, pathlib.Path))
        print(f"worker {name} pid={os.getpid()} wall_s={time.monotonic() - started:.3f} "
              f"peak_rss_bytes={peak_rss()} "
              f"peak_gpu_reserved_bytes={device}", flush=True)
        connection.close()


class TaskExecutor:
    """Spawn tasks independently, admitting their remaining allocations against live memory."""
    def __init__(self):
        self.pending = []
        self.queue = deque()
        self.priority_queue = deque()
        self.active = []
        self.condition = threading.Condition(threading.RLock())
        self.closed = False
        self.failure = None
        self.context = multiprocessing.get_context("spawn")
        self.thread = threading.Thread(target=self._run)
        self.previous_sigterm = None
        if threading.current_thread() is threading.main_thread():
            self.previous_sigterm = signal.signal(signal.SIGTERM, self._sigterm)
        self.thread.start()

    def submit(self, function, *args, estimates=FIT_BYTES, priority=False):
        future = concurrent.futures.Future()
        with self.condition:
            if self.closed or self.failure:
                raise RuntimeError("executor is closed or failed") from self.failure
            self.pending.append(future)
            item = (future, function, args, estimates)
            (self.priority_queue if priority else self.queue).append(item)
            self.condition.notify_all()
        return future

    def _run(self):
        try:
            self._schedule()
        except BaseException as error:
            with self.condition:
                self.failure = error
                for future in list(self.pending):
                    if not future.done():
                        future.set_exception(error)
                self.queue.clear()
                self.priority_queue.clear()
                self.pending.clear()
        finally:
            with self.condition:
                for process, connection, future, estimates in self.active:
                    if self.failure:
                        self._kill(process)
                    process.join()
                    connection.close()
                    process.close()
                self.active.clear()

    @staticmethod
    def _kill(process):
        if hasattr(os, "killpg"):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        if process.is_alive():
            process.kill()

    def _sigterm(self, number, frame):
        with self.condition:
            for process, *_ in self.active:
                self._kill(process)
        raise SystemExit(128 + number)

    def _schedule(self):
        next_query = 0
        query_failure_since = None
        last_query_failure_log = None
        last_wait_log = None
        while True:
            with self.condition:
                if self.failure:
                    raise self.failure
                for item in list(self.active):
                    process, connection, future, estimates = item
                    if connection.poll():
                        try:
                            success, result = connection.recv()
                        except EOFError:
                            success, result = False, f"worker {process.pid} exited without a result"
                        process.join()
                        self.active.remove(item)
                        connection.close()
                        process.close()
                        if not success:
                            raise RuntimeError(result)
                        future.set_result(result)
                        self.pending.remove(future)
                    elif not process.is_alive():
                        if connection.poll():
                            continue
                        raise RuntimeError(f"worker {process.pid} exited with code {process.exitcode}")
                if self.closed and not self.queue and not self.priority_queue and not self.active:
                    return
                if (self.queue or self.priority_queue) and time.monotonic() >= next_query:
                    query_error = None
                    self.condition.release()
                    try:
                        free = available_bytes(gpu=True)
                        gpu = gpu_resident_bytes()
                    except (subprocess.TimeoutExpired, OSError, subprocess.CalledProcessError) as error:
                        query_error = error
                    finally:
                        self.condition.acquire()
                    if self.failure:
                        raise self.failure
                    now = time.monotonic()
                    next_query = now + 2
                    if query_error is not None:
                        if query_failure_since is None:
                            query_failure_since = now
                        if now - query_failure_since >= GPU_QUERY_FAILURE_SECONDS:
                            raise RuntimeError(f"GPU memory queries failed for {GPU_QUERY_FAILURE_SECONDS} "
                                               "seconds of consecutive failures; worker admission unavailable") from query_error
                        if last_query_failure_log is None or now - last_query_failure_log >= GPU_QUERY_LOG_SECONDS:
                            print(f"GPU memory unknown this cycle: {query_error}; worker admission paused, retrying", flush=True)
                            last_query_failure_log = now
                        self.condition.wait(timeout=0.1)
                        continue
                    query_failure_since = None
                    last_query_failure_log = None
                    free = projected_available(free, [(estimates, resident_bytes(process.pid, gpu))
                                               for process, _, _, estimates in self.active])
                    for queued in [*self.priority_queue, *self.queue]:
                        if len(self.active) >= cores_available():
                            break
                        future, function, args, estimates = queued
                        if not worker_capacity(free, estimates if self.active else (0,) * 3, FLOORS, 1):
                            continue
                        (self.priority_queue if queued in self.priority_queue else self.queue).remove(queued)
                        print(f"admit {function_name(function)} projected_available={free} reservation={estimates}", flush=True)
                        receive, send = self.context.Pipe(duplex=False)
                        process = self.context.Process(target=_task, args=(send, function, args, estimates, os.getpid()))
                        try:
                            process.start()
                        except BaseException:
                            receive.close()
                            send.close()
                            raise
                        send.close()
                        self.active.append((process, receive, future, estimates))
                        free = projected_available(free, [(estimates, (0,) * 3)])
                    if (self.queue or self.priority_queue) and not self.active:
                        raise RuntimeError(f"worker memory admission failed: available={free}, "
                                           f"required={[item[3] for item in [*self.priority_queue, *self.queue]]}, floors={FLOORS}")
                    if (self.queue or self.priority_queue) and last_wait_log is None:
                        last_wait_log = time.monotonic()
                    if (self.queue or self.priority_queue) and time.monotonic() - last_wait_log >= 10:
                        print(f"admission wait projected_available={free} queued={len(self.queue) + len(self.priority_queue)} "
                              f"active={len(self.active)}", flush=True)
                        last_wait_log = time.monotonic()
                if not self.queue and not self.priority_queue:
                    last_wait_log = None
                self.condition.wait(timeout=0.1)

    def __enter__(self):
        return self

    def __exit__(self, *error):
        with self.condition:
            self.closed = True
            if error[0]:
                self.failure = error[1]
                for future, *_ in [*self.priority_queue, *self.queue]:
                    future.cancel()
                self.queue.clear()
                self.priority_queue.clear()
            self.condition.notify_all()
        self.thread.join()
        if self.previous_sigterm is not None:
            signal.signal(signal.SIGTERM, self.previous_sigterm)
        if self.failure and not error[0]:
            raise self.failure
