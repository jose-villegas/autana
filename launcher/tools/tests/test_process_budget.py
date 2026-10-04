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


class WorkerTests(unittest.TestCase):
    def test_worker_failure_reaches_parent(self):
        from unittest.mock import patch
        from r3d.process_budget import FitExecutor
        with patch.object(FitExecutor, "reserve"):
            with FitExecutor() as pool:
                with self.assertRaisesRegex(RuntimeError, "worker sentinel failure"):
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
