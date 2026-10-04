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
        from r3d.process_budget import FitExecutor
        with patch("r3d.process_budget.available_bytes", return_value=(1 << 60,) * 4):
            with self.assertRaisesRegex(RuntimeError, "worker sentinel failure"):
                with FitExecutor() as pool:
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
        self.assertEqual(projected_available((100, 100, 100, 100),
                         [((80, 80, 40, 80), (60, 60, 30, 60))]), (80, 80, 90, 80))

    def test_not_started_reserves_full_estimate(self):
        from r3d.process_budget import projected_available
        self.assertEqual(projected_available((100,) * 4, [((80,) * 4, (0,) * 4)]), (20,) * 4)

    def test_over_estimate_does_not_invent_available_memory(self):
        from r3d.process_budget import projected_available
        self.assertEqual(projected_available((100,) * 4, [((20,) * 4, (80,) * 4)]), (100,) * 4)

    def test_admission_failure_without_running_work_is_immediate(self):
        from unittest.mock import patch
        from r3d.process_budget import FitExecutor
        with patch('r3d.process_budget.available_bytes', return_value=(0,) * 4):
            with self.assertRaisesRegex(RuntimeError, 'memory admission'):
                with FitExecutor() as executor:
                    executor.submit(fail_worker).result(timeout=10)


def identity_worker(value, seconds=0):
    import os
    import time
    time.sleep(seconds)
    return value, os.getpid()


class SchedulerTests(ProcessTests):
    def test_spawned_prepares_and_fits_have_independent_pids(self):
        import os
        from unittest.mock import patch
        from r3d.process_budget import FitExecutor, PREPARE_BYTES
        with patch('r3d.process_budget.available_bytes', return_value=(1 << 60,) * 4):
            with FitExecutor() as executor:
                prepares = [executor.submit(identity_worker, name, .1, estimates=PREPARE_BYTES)
                            for name in ('lite', 'full')]
                fits = []
                for prepare in prepares:
                    fits.append(executor.submit(identity_worker, prepare.result(timeout=10)[0]))
                values = [future.result(timeout=10) for future in [*prepares, *fits]]
                self.assertEqual([value[0] for value in values], ['lite', 'full', 'lite', 'full'])
                self.assertEqual(len({value[1] for value in values}), 4)
                self.assertNotIn(os.getpid(), [value[1] for value in values])

    def test_busy_workers_make_progress_without_admission_timeout(self):
        from unittest.mock import patch
        from r3d.process_budget import FitExecutor, FIT_BYTES, FLOORS
        free = tuple(a + b for a, b in zip(FIT_BYTES, FLOORS))
        with patch('r3d.process_budget.available_bytes', return_value=free), \
                patch('r3d.process_budget.resident_bytes', return_value=(0,) * 4):
            with FitExecutor() as executor:
                first = executor.submit(identity_worker, 'first', .3)
                second = executor.submit(identity_worker, 'second')
                self.assertEqual(first.result(timeout=10)[0], 'first')
                self.assertEqual(second.result(timeout=10)[0], 'second')

    def test_measurement_can_finish_before_remaining_fit(self):
        from unittest.mock import patch
        from r3d.process_budget import FitExecutor
        with patch('r3d.process_budget.available_bytes', return_value=(1 << 60,) * 4), \
                patch('r3d.process_budget.cores_available', return_value=2):
            with FitExecutor() as executor:
                slow = executor.submit(identity_worker, 'fit', 3)
                measurement = executor.submit(identity_worker, 'measure', priority=True)
                self.assertEqual(measurement.result(timeout=10)[0], 'measure')
                self.assertFalse(slow.done())
                slow.result(timeout=10)
