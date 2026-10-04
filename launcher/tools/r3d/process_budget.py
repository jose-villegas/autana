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
from collections import deque

GIB = 1024 ** 3
FLOORS = (GIB, 3 * GIB, GIB // 2, GIB // 4)
FIT_BYTES = (2 * GIB, 2 * GIB, GIB, 2 * GIB)
# Prepare reserves the runner's approximately 5 GiB peak plus allocator and pose-start headroom.
PREPARE_BYTES = (6 * GIB, 6 * GIB, 0, 6 * GIB)
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


def _task(connection, function, args):
    if hasattr(os, "setsid"):
        os.setsid()
    try:
        connection.send((True, function(*args)))
    except BaseException:
        detail = traceback.format_exc()
        print(detail, flush=True)
        connection.send((False, detail))
    finally:
        connection.close()


class FitExecutor:
    """Spawn tasks independently, admitting their remaining allocations against live memory."""
    def __init__(self):
        self.pending = []
        self.queue = deque()
        self.active = []
        self.condition = threading.Condition(threading.RLock())
        self.closed = False
        self.failure = None
        self.context = multiprocessing.get_context("spawn")
        self.thread = threading.Thread(target=self._run)
        self.thread.start()

    def submit(self, function, *args, estimates=FIT_BYTES, priority=False):
        future = concurrent.futures.Future()
        with self.condition:
            if self.closed:
                raise RuntimeError("executor is closed")
            self.pending.append(future)
            (self.queue.appendleft if priority else self.queue.append)((future, function, args, estimates))
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
            for process, connection, future, estimates in self.active:
                if self.failure:
                    if hasattr(os, "killpg"):
                        try:
                            os.killpg(process.pid, signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                    elif process.is_alive():
                        process.terminate()
                    process.join(timeout=5)
                    if hasattr(os, "killpg"):
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    if process.is_alive():
                        process.kill()
                process.join()
                connection.close()
                process.close()

    def _schedule(self):
        next_query = 0
        while True:
            with self.condition:
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
                    while self.queue and len(self.active) < cores_available():
                        future, function, args, estimates = self.queue[0]
                        if not worker_capacity(free, estimates, FLOORS, 1):
                            if not self.active:
                                raise RuntimeError(f"worker memory admission failed: available={free}, "
                                                   f"required={estimates}, floors={FLOORS}")
                            break
                        self.queue.popleft()
                        receive, send = self.context.Pipe(duplex=False)
                        process = self.context.Process(target=_task, args=(send, function, args))
                        try:
                            process.start()
                        except BaseException:
                            receive.close()
                            send.close()
                            raise
                        send.close()
                        self.active.append((process, receive, future, estimates))
                        free = tuple(value - estimate for value, estimate in zip(free, estimates))
                self.condition.wait(timeout=0.1)

    def __enter__(self):
        return self

    def __exit__(self, *error):
        with self.condition:
            self.closed = True
            if error[0]:
                for future, *_ in self.queue:
                    future.cancel()
                self.queue.clear()
            self.condition.notify_all()
        self.thread.join()
        if self.failure and not error[0]:
            raise self.failure
