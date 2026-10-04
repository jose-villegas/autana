"""Process admission against available host, cgroup and device memory."""
import concurrent.futures
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
FLOORS = (GIB, 3 * GIB, GIB // 2, GIB // 4)
FIT_BYTES = (2 * GIB, 2 * GIB, GIB, 2 * GIB)
# Prepare includes the measured base peak and pose scratch; the reservation bounds its nested pool.
PREPARE_BYTES = (7 * GIB, 7 * GIB, 0, 7 * GIB)
TASK_RESERVATION = None
POSE_POOL_PEAK_BYTES = 0
SMOKE_PREPARE_BYTES = (2 * GIB, 2 * GIB, 0, 2 * GIB)


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
    wsl = next(int(line.split()[1]) * 1024 for line in pathlib.Path("/proc/meminfo").read_text().splitlines()
               if line.startswith("MemAvailable:"))
    host = unlimited
    powershell = pathlib.Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")
    if powershell.exists():
        host = int(subprocess.check_output([str(powershell), "-NoProfile", "-Command",
                    "(Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory).AvailableBytes"], timeout=30))
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
    return wsl, host, device, cgroup


def cores_available():
    return len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else os.cpu_count() or 1


def projected_available(available, workers):
    """Live counters already include resident allocations; reserve only their remainder."""
    return tuple(free - sum(max(0, estimate[index] - resident[index])
                           for estimate, resident in workers)
                 for index, free in enumerate(available))


def resident_bytes(pid, gpu):
    try:
        rss = next(int(line.split()[1]) * 1024
                   for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines()
                   if line.startswith("VmRSS:"))
    except (OSError, StopIteration):
        rss = 0
    return rss, rss, gpu.get(pid, 0), rss


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
    try:
        return next(int(line.split()[1]) * 1024
                    for line in pathlib.Path(f"/proc/{pid or os.getpid()}/status").read_text().splitlines()
                    if line.startswith("VmHWM:"))
    except (OSError, StopIteration):
        return 0


def _task(connection, function, args, estimates=FIT_BYTES, parent_pid=None):
    global TASK_RESERVATION
    parent_death_signal(parent_pid if parent_pid is not None else os.getppid())
    if hasattr(os, "setsid"):
        os.setsid()
    TASK_RESERVATION = estimates
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
        name = function.__name__ + ":" + ",".join(str(arg) for arg in args if isinstance(arg, pathlib.Path))
        print(f"worker {name} pid={os.getpid()} wall_s={time.monotonic() - started:.3f} "
              f"peak_rss_bytes={peak_rss()} pose_peak_rss_bytes={POSE_POOL_PEAK_BYTES} "
              f"peak_gpu_reserved_bytes={device}", flush=True)
        connection.close()


class FitExecutor:
    """Spawn tasks independently, admitting their remaining allocations against live memory."""
    def __init__(self):
        self.pending = []
        self.queue = deque()
        self.priorities = set()
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
            if self.closed:
                raise RuntimeError("executor is closed")
            self.pending.append(future)
            item = (future, function, args, estimates)
            if priority:
                index = next((index for index, queued in enumerate(self.queue)
                              if queued[0] not in self.priorities), len(self.queue))
                self.queue.insert(index, item)
                self.priorities.add(future)
            else:
                self.queue.append(item)
            self.condition.notify_all()
        return future

    def _run(self):
        try:
            self._schedule()
        except BaseException as error:
            self.failure = error
            with self.condition:
                for future in self.pending:
                    if not future.done():
                        future.set_exception(error)
                self.queue.clear()
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
                    elif not process.is_alive():
                        raise RuntimeError(f"worker {process.pid} exited with code {process.exitcode}")
                if self.closed and not self.queue and not self.active:
                    return
                if self.queue and time.monotonic() >= next_query:
                    free = available_bytes(gpu=True)
                    gpu = gpu_resident_bytes()
                    free = projected_available(free, [(estimates, resident_bytes(process.pid, gpu))
                                               for process, _, _, estimates in self.active])
                    next_query = time.monotonic() + 2
                    for queued in list(self.queue):
                        if len(self.active) >= cores_available():
                            break
                        future, function, args, estimates = queued
                        if not worker_capacity(free, estimates, FLOORS, 1):
                            continue
                        self.queue.remove(queued)
                        self.priorities.discard(future)
                        print(f"admit {function.__name__} projected_available={free} reservation={estimates}", flush=True)
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
                        free = tuple(value - estimate for value, estimate in zip(free, estimates))
                    if self.queue and not self.active:
                        raise RuntimeError(f"worker memory admission failed: available={free}, "
                                           f"required={[item[3] for item in self.queue]}, floors={FLOORS}")
                    if self.queue and last_wait_log is None:
                        last_wait_log = time.monotonic()
                    if self.queue and time.monotonic() - last_wait_log >= 10:
                        print(f"admission wait projected_available={free} queued={len(self.queue)} "
                              f"active={len(self.active)}", flush=True)
                        last_wait_log = time.monotonic()
                if not self.queue:
                    last_wait_log = None
                self.condition.wait(timeout=0.1)

    def __enter__(self):
        return self

    def __exit__(self, *error):
        with self.condition:
            self.closed = True
            if error[0]:
                self.failure = error[1]
                for future, *_ in self.queue:
                    future.cancel()
                self.queue.clear()
            self.condition.notify_all()
        self.thread.join()
        if self.previous_sigterm is not None:
            signal.signal(signal.SIGTERM, self.previous_sigterm)
        if self.failure and not error[0]:
            raise self.failure
