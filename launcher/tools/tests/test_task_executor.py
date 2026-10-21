"""GPU stage admission, stalled queries and ordered worker completion."""
import pathlib
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from test_process_budget import identity_worker, handshake_worker, fail_worker, exit_worker, reservation_worker

class ProcessTests(unittest.TestCase):
    def setUp(self):
        from unittest.mock import patch
        driver = patch('r3d.task_executor._nvml')
        driver.start().return_value.free_bytes.return_value = 1 << 60
        self.addCleanup(driver.stop)
        counter = patch('r3d.task_executor.gpu_resident_bytes', return_value={})
        counter.start()
        self.addCleanup(counter.stop)


class WorkerTests(ProcessTests):
    def test_worker_failure_reaches_parent(self):
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor
        with patch("r3d.task_executor.available_bytes", return_value=(1 << 60,) * 3):
            with self.assertRaisesRegex(RuntimeError, "worker sentinel failure"):
                with TaskExecutor() as pool:
                    pool.submit(fail_worker).result(timeout=30)

    def test_sweep_consumes_out_of_order_completion_in_point_order(self):
        import concurrent.futures
        import tempfile
        try:
            from r3d.fitted_variant import run_sweep_points, sweep_rows, point_name
        except ImportError:
            self.skipTest("needs NumPy")
        points = [{"budget": 1, "cost_weight": 0}, {"budget": 2, "cost_weight": 0}]
        futures = [concurrent.futures.Future(), concurrent.futures.Future()]
        class Executor:
            def submit(self, function, point, directory, **kwargs):
                return futures[point["budget"] - 1]
        with tempfile.TemporaryDirectory() as directory:
            pending = {}
            run_sweep_points(directory, points, None, executor=Executor(), deferred=pending)
            futures[1].set_result({"triangles": 2})
            futures[0].set_result({"triangles": 1})
            seen = []
            def consume(point, directory):
                seen.append(point["budget"])
                return pending[point_name(point)].result()
            self.assertEqual(run_sweep_points(directory, points, consume), 2)
            self.assertEqual(seen, [1, 2])
            self.assertEqual([row["triangles"] for row in sweep_rows(directory, points)], [1, 2])
            self.assertEqual(run_sweep_points(directory, points, consume), 0)

    def test_failed_sweep_has_no_resume_marker(self):
        import tempfile
        try:
            from r3d.fitted_variant import run_sweep_points
        except ImportError:
            self.skipTest("needs NumPy")
        with tempfile.TemporaryDirectory() as directory:
            def fail(point, path):
                raise RuntimeError("fit failed")
            with self.assertRaisesRegex(RuntimeError, "fit failed"):
                run_sweep_points(directory, [{"budget": 1, "cost_weight": 0}], fail)
            self.assertFalse(list(pathlib.Path(directory).rglob("result.json")))


class SchedulerTests(ProcessTests):
    def test_spawned_prepares_and_fits_have_independent_pids(self):
        import os
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor
        from r3d.process_budget import PREPARE_BYTES
        with patch('r3d.task_executor.available_bytes', return_value=(1 << 60,) * 3):
            with TaskExecutor() as executor:
                prepares = [executor.submit(identity_worker, name, estimates=PREPARE_BYTES)
                            for name in ('lite', 'full')]
                fits = []
                for prepare in prepares:
                    fits.append(executor.submit(identity_worker, prepare.result(timeout=10)[0]))
                values = [future.result(timeout=10) for future in [*prepares, *fits]]
                self.assertEqual([value[0] for value in values], ['lite', 'full', 'lite', 'full'])
                self.assertEqual(len({value[1] for value in values}), 4)
                self.assertNotIn(os.getpid(), [value[1] for value in values])


class ReservationPropagationTests(ProcessTests):
    def test_spawned_task_receives_its_own_reservation(self):
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor
        from r3d.process_budget import GIB
        reservation = (GIB, 0, GIB)
        with patch('r3d.task_executor.available_bytes', return_value=(1 << 60,) * 3):
            with TaskExecutor() as executor:
                self.assertEqual(executor.submit(reservation_worker, estimates=reservation).result(timeout=10), reservation)


class FailureStateTests(ProcessTests):
    def test_admission_failure_without_running_work_is_immediate(self):
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor
        with patch('r3d.task_executor.available_bytes', return_value=(0,) * 3):
            with self.assertRaisesRegex(RuntimeError, 'memory admission'):
                with TaskExecutor() as executor:
                    executor.submit(fail_worker).result(timeout=10)

    def test_submit_after_scheduler_failure_raises(self):
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor
        with patch('r3d.task_executor.available_bytes', return_value=(0,) * 3):
            executor = TaskExecutor()
            try:
                with self.assertRaises(RuntimeError):
                    executor.submit(identity_worker, 'first').result(timeout=10)
                with self.assertRaisesRegex(RuntimeError, 'failed') as caught:
                    executor.submit(identity_worker, 'late')
                self.assertIs(caught.exception.__cause__, executor.failure)
            finally:
                with self.assertRaises(RuntimeError):
                    executor.__exit__(None, None, None)

    def test_first_task_needs_only_floors(self):
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor, FLOORS
        with patch('r3d.task_executor.available_bytes', return_value=FLOORS):
            with TaskExecutor() as executor:
                self.assertEqual(executor.submit(identity_worker, 'first').result(timeout=10)[0], 'first')


class HandshakeSchedulerTests(ProcessTests):
    def exercise_residency(self, resident):
        import threading
        from unittest.mock import patch
        from r3d import task_executor as budget
        context = __import__('multiprocessing').get_context('spawn')
        started, release, second_started = [context.Event() for _ in range(3)]
        queried = threading.Event()
        calls = []
        free = tuple(a + b for a, b in zip(budget.FIT_BYTES, budget.FLOORS))
        def available(gpu=False):
            calls.append(None)
            if len(calls) >= 2:
                queried.set()
            return free
        with patch.object(budget, 'available_bytes', side_effect=available), \
                patch.object(budget, 'resident_bytes', return_value=resident), \
                patch.object(budget, 'cores_available', return_value=2):
            with budget.TaskExecutor() as executor:
                try:
                    with executor.condition:
                        first = executor.submit(handshake_worker, started, release, 'first')
                        second = executor.submit(handshake_worker, second_started, release, 'second')
                    self.assertTrue(started.wait(10))
                    if any(resident):
                        self.assertTrue(second_started.wait(10))
                        self.assertFalse(first.done())
                    else:
                        self.assertTrue(queried.wait(10))
                        with executor.condition:
                            self.assertEqual(len(executor.active), 1)
                        self.assertFalse(second_started.is_set())
                    release.set()
                    self.assertEqual(first.result(timeout=10)[0], 'first')
                    self.assertEqual(second.result(timeout=10)[0], 'second')
                    self.assertTrue(second_started.is_set())
                finally:
                    release.set()

    def test_nonresident_reservation_blocks_second_worker(self):
        self.exercise_residency((0,) * 3)

    def test_resident_worker_leaves_room_for_second(self):
        from r3d.task_executor import FIT_BYTES
        self.exercise_residency(FIT_BYTES)

    def test_done_callback_can_submit(self):
        import subprocess
        import tempfile
        code = """
import concurrent.futures, sys
from unittest.mock import patch
sys.path.insert(0, sys.argv[1])
from r3d.task_executor import TaskExecutor

if __name__ == '__main__':
    chained = concurrent.futures.Future()
    with patch('r3d.task_executor.available_bytes', return_value=(1 << 60,) * 3), \
            patch('r3d.task_executor.gpu_resident_bytes', return_value={}), \
            patch('r3d.task_executor._nvml') as driver:
        driver.return_value.free_bytes.return_value = 1 << 60
        with TaskExecutor() as executor:
            def completed(future):
                followup = executor.submit(abs, -2)
                followup.add_done_callback(lambda result: chained.set_result(result.result()))
            executor.submit(abs, -1).add_done_callback(completed)
            assert chained.result(timeout=10) == 2
"""
        with tempfile.TemporaryDirectory() as directory:
            script = pathlib.Path(directory) / 'callback.py'
            script.write_text(code)
            result = subprocess.run([sys.executable, str(script), str(pathlib.Path(__file__).resolve().parents[1])],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(sys.platform == 'linux', 'Linux abrupt worker exit')
    def test_exit_without_result_fails_promptly(self):
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor
        with patch('r3d.task_executor.available_bytes', return_value=(1 << 60,) * 3):
            with self.assertRaisesRegex(RuntimeError, 'worker .* (exited|without a result)'):
                with TaskExecutor() as executor:
                    executor.submit(exit_worker).result(timeout=10)

    def test_core_cap_and_priority_order(self):
        import threading
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor
        context = __import__('multiprocessing').get_context('spawn')
        started, release = context.Event(), context.Event()
        order = []
        core_checked = threading.Event()
        core_calls = []
        def cores():
            core_calls.append(None)
            if len(core_calls) >= 2:
                core_checked.set()
            return 1
        with patch('r3d.task_executor.available_bytes', return_value=(1 << 60,) * 3), \
                patch('r3d.task_executor.cores_available', side_effect=cores):
            with TaskExecutor() as executor:
                try:
                    first = executor.submit(handshake_worker, started, release, 'first')
                    self.assertTrue(started.wait(10))
                    with executor.condition:
                        ordinary = executor.submit(identity_worker, 'ordinary')
                        priority = executor.submit(identity_worker, 'priority', priority=True)
                        ordinary.add_done_callback(lambda f: order.append(f.result()[0]))
                        priority.add_done_callback(lambda f: order.append(f.result()[0]))
                    self.assertTrue(core_checked.wait(10))
                    with executor.condition:
                        self.assertEqual(len(executor.active), 1)
                        self.assertFalse(ordinary.done())
                        self.assertFalse(priority.done())
                    release.set()
                    first.result(timeout=10)
                    ordinary.result(timeout=10)
                    self.assertEqual(order, ['priority', 'ordinary'])
                finally:
                    release.set()

    def test_big_task_does_not_block_small_task(self):
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor
        from r3d.process_budget import GIB, FLOORS
        context = __import__('multiprocessing').get_context('spawn')
        started, release = context.Event(), context.Event()
        free = tuple(floor + 3 * GIB for floor in FLOORS)
        with patch('r3d.task_executor.available_bytes', return_value=free), \
                patch('r3d.task_executor.resident_bytes', return_value=(0,) * 3):
            with TaskExecutor() as executor:
                try:
                    first = executor.submit(handshake_worker, started, release, 'first', estimates=(2 * GIB,) * 3)
                    self.assertTrue(started.wait(10))
                    with executor.condition:
                        big = executor.submit(identity_worker, 'big', estimates=(4 * GIB,) * 3)
                        small = executor.submit(identity_worker, 'small', estimates=(GIB,) * 3)
                    self.assertEqual(small.result(timeout=10)[0], 'small')
                    self.assertFalse(big.done())
                    release.set()
                    first.result(timeout=10)
                    self.assertEqual(big.result(timeout=10)[0], 'big')
                finally:
                    release.set()

    def test_parent_error_kills_running_worker(self):
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor
        context = __import__('multiprocessing').get_context('spawn')
        started, release, ended = context.Event(), context.Event(), context.Event()
        with patch('r3d.task_executor.available_bytes', return_value=(1 << 60,) * 3):
            with self.assertRaisesRegex(ValueError, 'parent failure'):
                with TaskExecutor() as executor:
                    executor.submit(handshake_worker, started, release, 'first', ended)
                    self.assertTrue(started.wait(10))
                    raise ValueError('parent failure')
            self.assertFalse(ended.is_set())
            self.assertFalse(executor.thread.is_alive())
            self.assertEqual(executor.active, [])


class PartialTaskTests(ProcessTests):
    def test_sweep_partial_runs_and_logs(self):
        from functools import partial
        from unittest.mock import patch
        from r3d.task_executor import TaskExecutor
        with patch('r3d.task_executor.available_bytes', return_value=(1 << 60,) * 3):
            with TaskExecutor() as executor:
                self.assertEqual(executor.submit(partial(identity_worker, 'point')).result(timeout=10)[0], 'point')


class GpuSampleTests(unittest.TestCase):
    def test_staleness_boundary_and_failed_sample_never_reuses_success(self):
        from r3d import task_executor as budget
        sampler = budget._GpuSampler.__new__(budget._GpuSampler)
        sample = (123, {42: 456})
        sampled_at = 10
        sampler.latest = sample, sampled_at, None
        boundary = sampled_at + budget.GPU_QUERY_STALE_SECONDS
        self.assertEqual(sampler.read(boundary), sample)
        with self.assertRaisesRegex(OSError, "stalled.*stale"):
            sampler.read(boundary + 1)
        sampler.latest = None, boundary, OSError("query failed")
        with self.assertRaisesRegex(OSError, "query failed"):
            sampler.read(boundary)
        sampler.latest = sample, boundary, None
        self.assertEqual(sampler.read(boundary), sample)


class QueryFailuresTests(unittest.TestCase):
    def test_window_expires_at_the_limit_not_before(self):
        from r3d.task_executor import QueryFailures, GPU_QUERY_FAILURE_SECONDS as limit
        failures = QueryFailures()
        self.assertFalse(failures.failed(0)[0])
        self.assertFalse(failures.failed(limit - 1)[0])
        self.assertTrue(failures.failed(limit)[0])

    def test_recovery_restarts_the_window(self):
        from r3d.task_executor import QueryFailures, GPU_QUERY_FAILURE_SECONDS as limit
        failures = QueryFailures()
        failures.failed(0)
        failures.failed(limit - 50)
        failures.recovered()
        self.assertFalse(failures.failed(limit - 40)[0])
        self.assertFalse(failures.failed(2 * limit - 41)[0])
        self.assertTrue(failures.failed(2 * limit - 40)[0])

    def test_warnings_are_rate_limited_and_reopen_after_recovery(self):
        from r3d.task_executor import QueryFailures, GPU_QUERY_LOG_SECONDS as window
        failures = QueryFailures()
        self.assertTrue(failures.failed(0)[1])
        self.assertFalse(failures.failed(window - 1)[1])
        self.assertTrue(failures.failed(window)[1])
        failures.recovered()
        self.assertTrue(failures.failed(window + 1)[1])


class NvmlRuntimeTests(unittest.TestCase):
    FAILURE_SECONDS = 0.6
    STALE_SECONDS = 0.15
    SAMPLE_SECONDS = 0.03

    def setUp(self):
        import os
        import shutil
        import subprocess
        import tempfile
        from unittest.mock import patch
        from r3d import task_executor as budget
        if sys.platform != "linux" or not shutil.which("cc"):
            self.skipTest("requires Linux and cc")
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        root = pathlib.Path(self.directory.name)
        self.control, self.entered = root / "control", root / "entered"
        self.control.write_text("answer")
        source = root / "nvml.c"
        source.write_text(r'''#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#define NVML_SUCCESS 0
#define NVML_ERROR_GPU_IS_LOST 15
#define NVML_ERROR_DRIVER_NOT_LOADED 9
#define NVML_ERROR_NO_PERMISSION 4
#define NVML_ERROR_NOT_FOUND 6
#define NVML_ERROR_INSUFFICIENT_SIZE 7
#define NVML_ERROR_NOT_SUPPORTED 3
#define FREE_BYTES (1ULL << 60)
#define PROCESS_PID 42
#define PROCESS_BYTES 123
struct memory { unsigned long long total, free, used; };
struct process { unsigned int pid; unsigned long long used; unsigned int gpu, compute; };
static int state(void) {
    char mode[32] = "answer";
    FILE *file = fopen(getenv("TEST_NVML_CONTROL"), "r");
    if (file) { fscanf(file, "%31s", mode); fclose(file); }
    if (!strcmp(mode, "lost")) return NVML_ERROR_GPU_IS_LOST;
    if (!strcmp(mode, "driver")) return NVML_ERROR_DRIVER_NOT_LOADED;
    if (!strcmp(mode, "permission")) return NVML_ERROR_NO_PERMISSION;
    if (!strcmp(mode, "missing")) return NVML_ERROR_NOT_FOUND;
    if (!strcmp(mode, "unsupported")) return NVML_ERROR_NOT_SUPPORTED;
    if (!strcmp(mode, "hang")) {
        file = fopen(getenv("TEST_NVML_ENTERED"), "w");
        if (file) fclose(file);
        do { usleep(1000); file = fopen(getenv("TEST_NVML_CONTROL"), "r");
            if (!file) break;
            fscanf(file, "%31s", mode); fclose(file);
        } while (!strcmp(mode, "hang"));
    }
    return NVML_SUCCESS;
}
int nvmlInit_v2(void) { return NVML_SUCCESS; }
int nvmlShutdown(void) { return NVML_SUCCESS; }
int nvmlDeviceGetHandleByIndex_v2(unsigned int index, void **handle) {
    *handle = (void *)1; return NVML_SUCCESS;
}
int nvmlDeviceGetMemoryInfo(void *handle, struct memory *info) {
    int result = state(); if (result) return result;
    info->total = info->free = FREE_BYTES; info->used = 0; return NVML_SUCCESS;
}
int nvmlDeviceGetComputeRunningProcesses_v3(void *handle, unsigned int *count, struct process *info) {
    int result = state(); if (result) return result;
    if (*count < 1) { *count = 1; return NVML_ERROR_INSUFFICIENT_SIZE; }
    *count = 1; info[0].pid = PROCESS_PID; info[0].used = PROCESS_BYTES;
    info[0].gpu = info[0].compute = 0; return NVML_SUCCESS;
}
''')
        library = root / "libnvidia-ml.so"
        subprocess.run(["cc", "-shared", "-fPIC", str(source), "-o", str(library)], check=True,
                       capture_output=True)
        settings = patch.multiple(budget, NVML_LIBRARY=str(library),
                                  GPU_QUERY_FAILURE_SECONDS=self.FAILURE_SECONDS,
                                  GPU_QUERY_STALE_SECONDS=self.STALE_SECONDS,
                                  GPU_QUERY_SAMPLE_SECONDS=self.SAMPLE_SECONDS)
        settings.start()
        self.addCleanup(settings.stop)
        environment = patch.dict(os.environ, TEST_NVML_CONTROL=str(self.control),
                                 TEST_NVML_ENTERED=str(self.entered))
        environment.start()
        self.addCleanup(environment.stop)
        self.addCleanup(lambda: self.control.write_text("answer"))

    def wait_for(self, predicate):
        import time
        deadline = time.monotonic() + 10
        while not predicate():
            if time.monotonic() >= deadline:
                self.fail("timed out waiting for runtime state")
            time.sleep(0.01)

    def test_hung_query_preserves_workers_reaps_and_recovers(self):
        import multiprocessing
        import time
        from r3d import task_executor as budget
        context = multiprocessing.get_context("spawn")
        started, release, anchor_started, anchor_release = [context.Event() for _ in range(4)]
        with budget.TaskExecutor() as executor:
            try:
                first = executor.submit(handshake_worker, started, release, "first", estimates=(0,) * 3)
                anchor = executor.submit(handshake_worker, anchor_started, anchor_release, "anchor", estimates=(0,) * 3)
                self.assertTrue(started.wait(10))
                self.assertTrue(anchor_started.wait(10))
                self.control.write_text("hang")
                self.wait_for(self.entered.exists)
                time.sleep(self.STALE_SECONDS * 2)
                queued = executor.submit(identity_worker, "queued", estimates=(0,) * 3)
                release.set()
                self.assertEqual(first.result(timeout=10)[0], "first")
                time.sleep(self.FAILURE_SECONDS * 2)
                self.assertIsNone(executor.failure)
                self.assertEqual(len(executor.active), 1)
                self.assertFalse(queued.done())
                self.control.write_text("answer")
                self.assertEqual(queued.result(timeout=10)[0], "queued")
                anchor_release.set()
                self.assertEqual(anchor.result(timeout=10)[0], "anchor")
            finally:
                self.control.write_text("answer")
                release.set()
                anchor_release.set()

    def test_hung_query_without_workers_expires_with_stall_message(self):
        import time
        from r3d import task_executor as budget
        self.control.write_text("hang")
        started = time.monotonic()
        try:
            with self.assertRaisesRegex(RuntimeError, "GPU.*stall"):
                with budget.TaskExecutor() as executor:
                    executor.submit(identity_worker, "queued", estimates=(0,) * 3).result(timeout=10)
            self.assertGreaterEqual(time.monotonic() - started, self.FAILURE_SECONDS)
            self.assertTrue(executor.gpu_sampler.thread.daemon)
        finally:
            self.control.write_text("answer")
            executor.gpu_sampler.thread.join(timeout=5)
            self.assertFalse(executor.gpu_sampler.thread.is_alive())

    def test_gpu_lost_is_fatal_immediately(self):
        from unittest.mock import patch
        from r3d import task_executor as budget
        for mode, name in (("lost", "GPU_IS_LOST"), ("driver", "DRIVER_NOT_LOADED"),
                           ("permission", "NO_PERMISSION"), ("missing", "NOT_FOUND")):
            with self.subTest(mode=mode):
                self.control.write_text(mode)
                with patch.multiple(budget, GPU_QUERY_FAILURE_SECONDS=60, _nvml_instance=None):
                    with self.assertRaisesRegex(RuntimeError, "GPU is gone.*" + name):
                        with budget.TaskExecutor() as executor:
                            executor.submit(identity_worker, "queued").result(timeout=5)

    def test_missing_library_is_fatal_immediately(self):
        from unittest.mock import patch
        from r3d import task_executor as budget
        with patch.object(budget, "NVML_LIBRARY", str(self.control.parent / "missing.so")):
            with self.assertRaisesRegex(RuntimeError, "GPU is gone.*library"):
                with budget.TaskExecutor() as executor:
                    executor.submit(identity_worker, "queued").result(timeout=5)

    def test_queries_return_bytes_and_unsupported_processes_are_empty(self):
        from r3d import task_executor as budget
        self.assertEqual(budget._nvml().free_bytes(), 1 << 60)
        self.assertEqual(budget.gpu_resident_bytes(), {42: 123})
        self.control.write_text("unsupported")
        self.assertEqual(budget.gpu_resident_bytes(), {})
