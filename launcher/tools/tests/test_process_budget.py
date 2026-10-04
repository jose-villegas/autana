"""Memory admission and ordered process results."""
import pathlib
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.process_budget import worker_capacity

class CapacityTests(unittest.TestCase):
    def test_limiting_resource(self):
        self.assertEqual(worker_capacity((100, 60, 90), (20, 10, 30), (20, 20, 0), 10), 3)
    def test_floor_is_preserved(self):
        self.assertEqual(worker_capacity((19, 100), (1, 10), (20, 20), 10), 0)
    def test_cpu_limit(self):
        self.assertEqual(worker_capacity((1000,), (10,), (0,), 3), 3)
    def test_unused_resource(self):
        self.assertEqual(worker_capacity((100, 0), (10, 0), (20, 0), 20), 8)
    def test_invalid_estimate(self):
        with self.assertRaises(ValueError):
            worker_capacity((100,), (-1,), (0,), 2)


def fail_worker():
    raise RuntimeError("worker sentinel failure")


class ProcessTests(unittest.TestCase):
    def setUp(self):
        from unittest.mock import patch
        counter = patch('r3d.process_budget.gpu_resident_bytes', return_value={})
        counter.start()
        self.addCleanup(counter.stop)


class WorkerTests(ProcessTests):
    def test_worker_failure_reaches_parent(self):
        from unittest.mock import patch
        from r3d.process_budget import TaskExecutor
        with patch("r3d.process_budget.available_bytes", return_value=(1 << 60,) * 3):
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
            def submit(self, function, point, directory):
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


class ProjectionTests(ProcessTests):
    def test_running_allocations_are_not_counted_twice(self):
        from r3d.process_budget import projected_available
        self.assertEqual(projected_available((100, 100, 100),
                         [((80, 40, 80), (60, 30, 60))]), (80, 90, 80))

    def test_not_started_reserves_full_estimate(self):
        from r3d.process_budget import projected_available
        self.assertEqual(projected_available((100,) * 3, [((80,) * 3, (0,) * 3)]), (20, 20, 20))

    def test_over_estimate_does_not_invent_available_memory(self):
        from r3d.process_budget import projected_available
        self.assertEqual(projected_available((100,) * 3, [((20,) * 3, (80,) * 3)]), (100,) * 3)

    def test_admission_failure_without_running_work_is_immediate(self):
        from unittest.mock import patch
        from r3d.process_budget import TaskExecutor
        with patch('r3d.process_budget.available_bytes', return_value=(0,) * 3):
            with self.assertRaisesRegex(RuntimeError, 'memory admission'):
                with TaskExecutor() as executor:
                    executor.submit(fail_worker).result(timeout=10)


def identity_worker(value):
    import os
    return value, os.getpid()


class SchedulerTests(ProcessTests):
    def test_spawned_prepares_and_fits_have_independent_pids(self):
        import os
        from unittest.mock import patch
        from r3d.process_budget import TaskExecutor, PREPARE_BYTES
        with patch('r3d.process_budget.available_bytes', return_value=(1 << 60,) * 3):
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



class CleanupTests(ProcessTests):
    @unittest.skipUnless(sys.platform == 'linux', 'Linux parent death signal')
    def test_worker_dies_when_parent_is_killed(self):
        import os
        import subprocess
        import tempfile
        import time
        with tempfile.TemporaryDirectory() as directory:
            marker = pathlib.Path(directory) / 'pid'
            code = """
import multiprocessing, pathlib, sys, time
sys.path.insert(0, sys.argv[1])
from r3d.process_budget import _task

def work():
    import os
    pathlib.Path(sys.argv[2]).write_text(str(os.getpid()))
    time.sleep(60)

if __name__ == '__main__':
    context = multiprocessing.get_context('spawn')
    receive, send = context.Pipe(False)
    process = context.Process(target=_task, args=(send, work, (), (0,) * 3))
    process.start()
    time.sleep(60)
"""
            script = pathlib.Path(directory) / 'parent.py'
            script.write_text(code)
            parent = subprocess.Popen([sys.executable, str(script), str(pathlib.Path(__file__).resolve().parents[1]), str(marker)])
            try:
                deadline = time.monotonic() + 5
                while not marker.exists() and time.monotonic() < deadline:
                    time.sleep(.05)
                self.assertTrue(marker.exists())
                pid = int(marker.read_text())
                parent.kill()
                parent.wait(timeout=5)
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    status = pathlib.Path(f'/proc/{pid}/status')
                    if not status.exists() or 'State:\tZ' in status.read_text():
                        break
                    time.sleep(.05)
                else:
                    self.fail('worker survived parent SIGKILL')
            finally:
                if parent.poll() is None:
                    parent.kill()
                parent.wait(timeout=5)
                if marker.exists():
                    try:
                        os.kill(int(marker.read_text()), 9)
                    except ProcessLookupError:
                        pass


class ReservationTests(unittest.TestCase):
    def test_pose_pool_stays_inside_reservation(self):
        try:
            from r3d.reference_render import reservation_pose_capacity
        except ImportError:
            self.skipTest('needs reference dependencies')
        self.assertEqual(reservation_pose_capacity(700, 300, 100, 10), 4)
        self.assertEqual(reservation_pose_capacity(700, 300, 100, 2), 2)
        self.assertEqual(reservation_pose_capacity(700, 690, 100, 10), 1)



def reservation_worker():
    from r3d.process_budget import task_reservation
    return task_reservation()


class ReservationPropagationTests(ProcessTests):
    def test_spawned_task_receives_its_own_reservation(self):
        from unittest.mock import patch
        from r3d.process_budget import TaskExecutor, GIB
        reservation = (GIB, 0, GIB)
        with patch('r3d.process_budget.available_bytes', return_value=(1 << 60,) * 3):
            with TaskExecutor() as executor:
                self.assertEqual(executor.submit(reservation_worker, estimates=reservation).result(timeout=10), reservation)


class FailureStateTests(ProcessTests):
    def test_submit_after_scheduler_failure_raises(self):
        from unittest.mock import patch
        from r3d.process_budget import TaskExecutor
        with patch('r3d.process_budget.available_bytes', return_value=(0,) * 3):
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
        from r3d.process_budget import TaskExecutor, FLOORS
        with patch('r3d.process_budget.available_bytes', return_value=FLOORS):
            with TaskExecutor() as executor:
                self.assertEqual(executor.submit(identity_worker, 'first').result(timeout=10)[0], 'first')


def handshake_worker(started, release, value, ended=None):
    import os
    started.set()
    try:
        if not release.wait(20):
            raise RuntimeError('handshake timed out')
        return value, os.getpid()
    finally:
        if ended is not None:
            ended.set()


def exit_worker():
    import os
    os._exit(7)


class HandshakeSchedulerTests(ProcessTests):
    def exercise_residency(self, resident):
        import threading
        from unittest.mock import patch
        from r3d import process_budget as budget
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
        from r3d.process_budget import FIT_BYTES
        self.exercise_residency(FIT_BYTES)

    def test_done_callback_can_submit(self):
        import subprocess
        import tempfile
        code = """
import concurrent.futures, sys
from unittest.mock import patch
sys.path.insert(0, sys.argv[1])
from r3d.process_budget import TaskExecutor

if __name__ == '__main__':
    chained = concurrent.futures.Future()
    with patch('r3d.process_budget.available_bytes', return_value=(1 << 60,) * 3), \
            patch('r3d.process_budget.gpu_resident_bytes', return_value={}):
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
        from r3d.process_budget import TaskExecutor
        with patch('r3d.process_budget.available_bytes', return_value=(1 << 60,) * 3):
            with self.assertRaisesRegex(RuntimeError, 'worker .* (exited|without a result)'):
                with TaskExecutor() as executor:
                    executor.submit(exit_worker).result(timeout=10)

    def test_core_cap_and_priority_order(self):
        import threading
        from unittest.mock import patch
        from r3d.process_budget import TaskExecutor
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
        with patch('r3d.process_budget.available_bytes', return_value=(1 << 60,) * 3), \
                patch('r3d.process_budget.cores_available', side_effect=cores):
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
        from r3d.process_budget import TaskExecutor, GIB, FLOORS
        context = __import__('multiprocessing').get_context('spawn')
        started, release = context.Event(), context.Event()
        free = tuple(floor + 3 * GIB for floor in FLOORS)
        with patch('r3d.process_budget.available_bytes', return_value=free), \
                patch('r3d.process_budget.resident_bytes', return_value=(0,) * 3):
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
        from r3d.process_budget import TaskExecutor
        context = __import__('multiprocessing').get_context('spawn')
        started, release, ended = context.Event(), context.Event(), context.Event()
        with patch('r3d.process_budget.available_bytes', return_value=(1 << 60,) * 3):
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
        from r3d.process_budget import TaskExecutor
        with patch('r3d.process_budget.available_bytes', return_value=(1 << 60,) * 3):
            with TaskExecutor() as executor:
                self.assertEqual(executor.submit(partial(identity_worker, 'point')).result(timeout=10)[0], 'point')
