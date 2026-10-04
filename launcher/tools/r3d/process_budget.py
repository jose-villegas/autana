"""Process admission against available host, cgroup and device memory."""
import concurrent.futures
import multiprocessing
import os
import pathlib
import subprocess
import time

GIB = 1024 ** 3
FLOORS = (GIB, 3 * GIB, GIB // 2, GIB // 4)
FIT_BYTES = (2 * GIB, 2 * GIB, GIB, 2 * GIB)
PREPARE_BYTES = (4 * GIB, 4 * GIB, 0, 4 * GIB)


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
                    "(Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory).AvailableBytes"]))
    device = unlimited
    if gpu:
        rows = subprocess.check_output(["nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"]).decode()
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


class FitExecutor:
    """Fresh CUDA processes; pending allocations remain reserved until their task finishes."""
    def __init__(self, timeout=1800):
        self.timeout = timeout
        self.pending = []
        self.pool = concurrent.futures.ProcessPoolExecutor(max_workers=cores_available(),
            mp_context=multiprocessing.get_context("spawn"), max_tasks_per_child=1)

    def reserve(self, estimates):
        deadline = time.monotonic() + self.timeout
        while True:
            active = [future for future in self.pending if not future.done()]
            for future in self.pending:
                if future.done():
                    future.result()
            free = tuple(value - len(active) * estimate for value, estimate in zip(available_bytes(gpu=True), FIT_BYTES))
            if worker_capacity(free, estimates, FLOORS, 1):
                return
            if time.monotonic() >= deadline:
                raise RuntimeError(f"worker memory admission timed out: available={free}, required={estimates}, floors={FLOORS}")
            time.sleep(0.2)

    def submit(self, function, *args):
        self.reserve(FIT_BYTES)
        future = self.pool.submit(function, *args)
        self.pending.append(future)
        return future

    def __enter__(self):
        return self

    def __exit__(self, *error):
        self.pool.shutdown(wait=True, cancel_futures=True)
