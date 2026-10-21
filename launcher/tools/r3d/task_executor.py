"""GPU stage admission using nonblocking NVML samples and shared process budgets."""
import ctypes
import os
import threading
import time

from r3d.process_budget import (FIT_BYTES, FLOORS, GPU_QUERY_FAILURE_SECONDS, GPU_QUERY_LOG_SECONDS,
                                QueryFailures, TaskExecutor as _ProcessExecutor, _task, available_bytes,
                                cores_available, function_name, projected_available, resident_bytes, worker_capacity)

_ADMISSION_POLL_SECONDS = 0.1
_ADMISSION_LOG_SECONDS = 10
GPU_QUERY_SAMPLE_SECONDS = 1
GPU_QUERY_STALE_SECONDS = 5
_GPU_QUERY_STOP_SECONDS = 0.1
NVML_LIBRARY = "libnvidia-ml.so.1"
NVML_DEVICE_INDEX = 0
NVML_SUCCESS = 0
NVML_ERROR_NOT_SUPPORTED = 3
NVML_ERROR_NO_PERMISSION = 4
NVML_ERROR_NOT_FOUND = 6
NVML_ERROR_INSUFFICIENT_SIZE = 7
NVML_ERROR_DRIVER_NOT_LOADED = 9
NVML_ERROR_GPU_IS_LOST = 15
NVML_VALUE_NOT_AVAILABLE = ctypes.c_ulonglong(-1).value
_NVML_GONE_CODES = {
    NVML_ERROR_GPU_IS_LOST: "GPU_IS_LOST",
    NVML_ERROR_DRIVER_NOT_LOADED: "DRIVER_NOT_LOADED",
    NVML_ERROR_NO_PERMISSION: "NO_PERMISSION",
    NVML_ERROR_NOT_FOUND: "NOT_FOUND",
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
            self.library = ctypes.CDLL(NVML_LIBRARY)
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
            function = getattr(self.library, name)
            function.argtypes = arguments
            function.restype = ctypes.c_int
        self.initialized = False
        self.handle = None
        self.gone = None

    def _check(self, result):
        if result in _NVML_GONE_CODES:
            self.gone = _GpuGone(f"GPU is gone: NVML {_NVML_GONE_CODES[result]}")
            raise self.gone
        if result != NVML_SUCCESS:
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
                self._check(self.library.nvmlDeviceGetHandleByIndex_v2(NVML_DEVICE_INDEX, ctypes.byref(handle)))
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
            if result == NVML_ERROR_NOT_SUPPORTED:
                return {}
            if result != NVML_ERROR_INSUFFICIENT_SIZE:
                self._check(result)
                return {process.pid: process.usedGpuMemory for process in (processes or ())[:count.value]
                        if process.usedGpuMemory != NVML_VALUE_NOT_AVAILABLE}
            processes = (_NvmlProcess * count.value)()


def _nvml():
    global _nvml_instance
    key = (os.getpid(), NVML_LIBRARY)
    with _nvml_lock:
        if _nvml_instance is None or _nvml_instance[0] != key:
            _nvml_instance = key, _Nvml()
        return _nvml_instance[1]


def gpu_resident_bytes():
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
            try:
                sample = (_nvml().free_bytes(), gpu_resident_bytes())
                self.latest = (sample, time.monotonic(), None)
            except (OSError, AttributeError) as error:
                self.latest = (None, time.monotonic(), error)
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


class TaskExecutor(_ProcessExecutor):
    """Preserve active workers while failed or stalled GPU queries pause admission."""

    def __init__(self):
        self.gpu_sampler = _GpuSampler()
        super().__init__()

    def _run(self):
        try:
            super()._run()
        finally:
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
            process.join()
            self.active.remove(item)
            connection.close()
            process.close()
            if not success:
                raise RuntimeError(result)
            future.set_result(result)
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
                    except (OSError, AttributeError) as error:
                        _, warn = failures.failed(now)
                        if queued and not self.active:
                            if admission_failure_since is None:
                                admission_failure_since = now
                            if now - admission_failure_since >= GPU_QUERY_FAILURE_SECONDS:
                                raise RuntimeError(f"GPU memory query stalled or failed for {GPU_QUERY_FAILURE_SECONDS} "
                                                   "seconds with no active workers; worker admission unavailable") from error
                        if warn:
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
