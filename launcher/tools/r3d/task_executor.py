"""GPU stage admission using nonblocking NVML samples and shared process budgets."""
import concurrent.futures
import ctypes
import os
import pathlib
import threading
import time

from r3d.process_budget import (FLOORS, GPU_QUERY_FAILURE_SECONDS,
                                QueryFailures, TaskExecutor as _ProcessExecutor, _task, available_bytes,
                                cores_available, function_name, projected_available,
                                resident_bytes, worker_capacity)

# A worker that has sent its result is given this long to exit before it is killed: a join without a bound
# under the executor's lock stalls every task behind it.
WORKER_EXIT_SECONDS = 30
# The environment variable naming the file a GPU stage's guard (scripts/lib/local_schedule.py) times progress
# by; progressed() touches it each time a task or bake finishes.
STAGE_PROGRESS_ENV = "AUTANA_STAGE_PROGRESS"
_ADMISSION_POLL_SECONDS = 0.1
_ADMISSION_LOG_SECONDS = 10
GPU_QUERY_SAMPLE_SECONDS = 1
GPU_QUERY_STALE_SECONDS = 5
_GPU_QUERY_STOP_SECONDS = 0.1
_NVML_LIBRARY = "libnvidia-ml.so.1"
_NVML_DEVICE_INDEX = 0
_NVML_SUCCESS = 0
_NVML_ERROR_NOT_SUPPORTED = 3
_NVML_ERROR_NO_PERMISSION = 4
_NVML_ERROR_NOT_FOUND = 6
_NVML_ERROR_INSUFFICIENT_SIZE = 7
_NVML_ERROR_DRIVER_NOT_LOADED = 9
_NVML_ERROR_GPU_IS_LOST = 15
_NVML_VALUE_NOT_AVAILABLE = ctypes.c_ulonglong(-1).value
_NVML_GONE_CODES = {
    _NVML_ERROR_GPU_IS_LOST: "GPU_IS_LOST",
    _NVML_ERROR_DRIVER_NOT_LOADED: "DRIVER_NOT_LOADED",
    _NVML_ERROR_NO_PERMISSION: "NO_PERMISSION",
    _NVML_ERROR_NOT_FOUND: "NOT_FOUND",
}
_nvml_instance = None
_nvml_lock = threading.Lock()


class _GpuGone(OSError):
    pass


class _NvmlMemory(ctypes.Structure):
    _fields_ = [(name, ctypes.c_ulonglong) for name in ("total", "free", "used")]


class _NvmlProcess(ctypes.Structure):
    _fields_ = [("pid", ctypes.c_uint), ("usedGpuMemory", ctypes.c_ulonglong),
                ("gpuInstanceId", ctypes.c_uint), ("computeInstanceId", ctypes.c_uint)]


class _Nvml:
    def __init__(self):
        try:
            self.library = ctypes.CDLL(_NVML_LIBRARY)
        except OSError as error:
            raise _GpuGone(f"GPU is gone: NVML library unavailable: {error}") from error
        signatures = {
            "nvmlInit_v2": [],
            "nvmlDeviceGetHandleByIndex_v2": [ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)],
            "nvmlDeviceGetMemoryInfo": [ctypes.c_void_p, ctypes.POINTER(_NvmlMemory)],
            "nvmlDeviceGetComputeRunningProcesses_v3": [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint),
                                                       ctypes.POINTER(_NvmlProcess)],
        }
        for name, arguments in signatures.items():
            try:
                function = getattr(self.library, name)
            except AttributeError as error:
                raise _GpuGone(f"GPU is gone: NVML symbol missing: {name}") from error
            function.argtypes = arguments
            function.restype = ctypes.c_int
        self.initialized = False
        self.handle = None
        self.gone = None

    def _check(self, result):
        if result in _NVML_GONE_CODES:
            self.gone = _GpuGone(f"GPU is gone: NVML {_NVML_GONE_CODES[result]}")
            raise self.gone
        if result != _NVML_SUCCESS:
            raise OSError(f"NVML query failed with code {result}")

    def device(self):
        if self.gone:
            raise self.gone
        with _nvml_lock:
            if not self.initialized:
                self._check(self.library.nvmlInit_v2())
                self.initialized = True
            if self.handle is None:
                handle = ctypes.c_void_p()
                self._check(self.library.nvmlDeviceGetHandleByIndex_v2(_NVML_DEVICE_INDEX, ctypes.byref(handle)))
                self.handle = handle
        return self.handle

    def free_bytes(self):
        memory = _NvmlMemory()
        self._check(self.library.nvmlDeviceGetMemoryInfo(self.device(), ctypes.byref(memory)))
        return memory.free

    def resident_bytes(self):
        device = self.device()
        count = ctypes.c_uint()
        processes = None
        while True:
            result = self.library.nvmlDeviceGetComputeRunningProcesses_v3(device, ctypes.byref(count), processes)
            if result == _NVML_ERROR_NOT_SUPPORTED:
                return {}
            if result != _NVML_ERROR_INSUFFICIENT_SIZE:
                self._check(result)
                return {process.pid: process.usedGpuMemory for process in (processes or ())[:count.value]
                        if process.usedGpuMemory != _NVML_VALUE_NOT_AVAILABLE}
            processes = (_NvmlProcess * count.value)()


def _nvml():
    global _nvml_instance
    with _nvml_lock:
        if _nvml_instance is None:
            _nvml_instance = _Nvml()
        return _nvml_instance


def _gpu_resident_bytes():
    return _nvml().resident_bytes()


class _GpuSampler:
    """Keep driver calls off admission and shutdown paths, including a wedged driver."""

    def __init__(self):
        self.stopped = threading.Event()
        self.latest = (None, None, OSError("GPU query stalled: awaiting first sample"))
        self.thread = threading.Thread(target=self._poll, daemon=True)
        self.thread.start()

    def _poll(self):
        while not self.stopped.is_set():
            sampled_at = time.monotonic()
            try:
                sample = (_nvml().free_bytes(), _gpu_resident_bytes())
                self.latest = (sample, sampled_at, None)
            except OSError as error:
                self.latest = (None, sampled_at, error)
                if isinstance(error, _GpuGone):
                    return
            if self.stopped.wait(GPU_QUERY_SAMPLE_SECONDS):
                return

    def read(self, now):
        sample, sampled_at, error = self.latest
        if error is not None:
            raise error
        if now - sampled_at > GPU_QUERY_STALE_SECONDS:
            raise OSError("GPU query stalled: latest sample is stale")
        return sample

    def stop(self):
        self.stopped.set()
        self.thread.join(timeout=_GPU_QUERY_STOP_SECONDS)


def progressed():
    """Tell the stage's guard, when one runs this, that a task finished; a guard's lost file never fails the work."""
    path = os.environ.get(STAGE_PROGRESS_ENV)
    if path:
        try:
            pathlib.Path(path).touch()
        except OSError:
            pass


def join_worker(process):
    """Wait WORKER_EXIT_SECONDS for a worker that has answered, then kill it."""
    process.join(WORKER_EXIT_SECONDS)
    if process.is_alive():
        print(f"worker {process.pid} still running {WORKER_EXIT_SECONDS} s after its result; killed", flush=True)
        _ProcessExecutor._kill(process)
        process.join()


class TaskExecutor(_ProcessExecutor):
    """Preserve active workers while failed or stalled GPU queries pause admission. Futures settle outside the
    lock: their done-callbacks may take locks of their own and submit, so settling them under it deadlocks
    against a caller that holds such a lock while it submits."""

    def __init__(self):
        # (future, result, error) reaped under the lock, settled by _settle() after it is released.
        self.settled = []
        self.gpu_sampler = _GpuSampler()
        super().__init__()

    def _settle(self):
        with self.condition:
            settled, self.settled = self.settled, []
        for future, result, error in settled:
            try:
                future.set_exception(error) if error else future.set_result(result)
            except concurrent.futures.InvalidStateError:
                pass  # cancelled meanwhile
            if not error:
                progressed()

    def _run(self):
        try:
            self._schedule()
        except BaseException as error:
            with self.condition:
                self.failure = error
                self.settled += [(future, None, error) for future in self.pending]
                self.queue.clear()
                self.priority_queue.clear()
                self.pending.clear()
        finally:
            self._settle()
            with self.condition:
                for process, connection, _future, _estimates in self.active:
                    if self.failure:
                        self._kill(process)
                    process.join()
                    connection.close()
                    process.close()
                self.active.clear()
            self.gpu_sampler.stop()

    def _reap(self):
        for item in tuple(self.active):
            process, connection, future, _ = item
            if not connection.poll():
                if process.is_alive() or connection.poll():
                    continue
                raise RuntimeError(f"worker {process.pid} exited with code {process.exitcode}")
            try:
                success, result = connection.recv()
            except EOFError:
                success, result = False, f"worker {process.pid} exited without a result"
            join_worker(process)
            self.active.remove(item)
            connection.close()
            process.close()
            if not success:
                raise RuntimeError(result)
            self.settled.append((future, result, None))
            self.pending.remove(future)

    def _start(self, item):
        future, function, arguments, estimates = item
        receive, send = self.context.Pipe(duplex=False)
        worker = self.context.Process(target=_task, args=(send, function, arguments, estimates, os.getpid()))
        try:
            worker.start()
        except BaseException:
            receive.close()
            raise
        finally:
            send.close()
        self.active.append((worker, receive, future, estimates))

    def _admit(self, free, gpu):
        reservations = [(cost, resident_bytes(worker.pid, gpu)) for worker, _, _, cost in self.active]
        free = projected_available(free, reservations)
        for queue in (self.priority_queue, self.queue):
            for item in tuple(queue):
                if len(self.active) >= cores_available():
                    return free
                estimate = item[3]
                required = estimate if self.active else (0,) * len(FLOORS)
                if worker_capacity(free, required, FLOORS, 1) == 0:
                    continue
                print(f"admit {function_name(item[1])} projected_available={free} reservation={estimate}", flush=True)
                queue.remove(item)
                self._start(item)
                free = projected_available(free, [(estimate, (0,) * len(FLOORS))])
        if (self.queue or self.priority_queue) and not self.active:
            required = [item[3] for item in (*self.priority_queue, *self.queue)]
            raise RuntimeError(f"worker memory admission failed: available={free}, required={required}, floors={FLOORS}")
        return free

    def _schedule(self):
        failures = QueryFailures()
        admission_failure_since = None
        next_admission = 0
        last_wait_log = None
        while True:
            self._settle()
            with self.condition:
                if self.failure:
                    raise self.failure
                self._reap()
                queued = len(self.priority_queue) + len(self.queue)
                if self.active or not queued:
                    admission_failure_since = None
                if self.closed and not queued and not self.active:
                    return
                if queued or self.active:
                    now = time.monotonic()
                    try:
                        device_free, gpu = self.gpu_sampler.read(now)
                    except _GpuGone as error:
                        raise RuntimeError(str(error)) from error
                    except OSError as error:
                        _, warn = failures.failed(now)
                        if queued and not self.active:
                            if admission_failure_since is None:
                                admission_failure_since = now
                            if now - admission_failure_since >= GPU_QUERY_FAILURE_SECONDS:
                                raise RuntimeError(f"GPU memory query stalled or failed for {GPU_QUERY_FAILURE_SECONDS} "
                                                   "seconds with no active workers; worker admission unavailable") from error
                        if queued and warn:
                            print(f"memory query failed: {error}; worker admission paused, retrying", flush=True)
                    else:
                        failures.recovered()
                        admission_failure_since = None
                        if queued and now >= next_admission:
                            host, _, cgroup = available_bytes()
                            free = self._admit((host, device_free, cgroup), gpu)
                            next_admission = now + GPU_QUERY_SAMPLE_SECONDS
                            if self.queue or self.priority_queue:
                                if last_wait_log is None:
                                    last_wait_log = now
                                if now - last_wait_log >= _ADMISSION_LOG_SECONDS:
                                    print(f"admission wait projected_available={free} "
                                          f"queued={len(self.queue) + len(self.priority_queue)} "
                                          f"active={len(self.active)}", flush=True)
                                    last_wait_log = now
                if not self.queue and not self.priority_queue:
                    last_wait_log = None
                self.condition.wait(timeout=_ADMISSION_POLL_SECONDS)
