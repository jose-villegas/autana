"""Checks the path-traced reference sweep's measures on synthetic images, and its memory and table helpers."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

try:
    import numpy as np

    from r3d import reference_sweep
    from r3d.reference_render import main as reference_main
except ImportError:
    np = None


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ReferenceSweepTest(unittest.TestCase):
    def test_relative_noise_is_the_seed_spread_over_the_mean(self):
        rng = np.random.default_rng(1)
        images = [np.full((16, 16, 3), 2.0) + rng.normal(0.0, 0.2, (16, 16, 3)) for _ in range(64)]
        self.assertAlmostEqual(reference_sweep.relative_noise(images), 0.1, delta=0.01)

    def test_identical_pictures_differ_by_nothing_and_different_ones_do(self):
        a = np.full((4, 4, 3), 100.0)
        self.assertEqual(reference_sweep.delta_e(a, a), 0.0)
        self.assertGreater(reference_sweep.delta_e(a, a + 30.0), 5.0)

    def test_an_all_black_image_has_no_relative_noise_rather_than_nan(self):
        self.assertEqual(reference_sweep.relative_noise([np.zeros((4, 4, 3)), np.zeros((4, 4, 3))]), 0.0)

    def test_the_memory_floor_stops_the_sweep_only_below_it(self):
        saved = reference_sweep.available_memory_gib
        try:
            reference_sweep.available_memory_gib = lambda: 5.0
            reference_sweep.require_memory(3.0, "stage")
            reference_sweep.available_memory_gib = lambda: 1.0
            with self.assertRaises(SystemExit):
                reference_sweep.require_memory(3.0, "stage")
        finally:
            reference_sweep.available_memory_gib = saved

    def test_the_host_memory_reading_is_a_positive_number_of_gib(self):
        self.assertGreater(reference_sweep.available_memory_gib(), 0.0)

    def test_measure_reports_the_cold_trace_apart_and_compares_depths_on_matching_seeds(self):
        calls = []

        def trace(spp, seed, depth):
            calls.append((spp, seed, depth))
            image = np.full((2, 2, 3), 1.0 + 0.01 * seed + (0.5 if depth == 24 else 0.0))
            return image, np.ones((2, 2))

        result = reference_sweep.measure(trace, [12, 24], [64, 256], 256, 2, lambda linear, covered: linear * 100)
        self.assertEqual(sorted(calls), sorted([(spp, seed, 12) for spp in (64, 256) for seed in (1, 2)] +
                                               [(256, seed, 24) for seed in (1, 2)]))
        self.assertEqual(calls[0], (64, 1, 12))
        self.assertEqual(result["cold_spp"], 64)
        cells = result["depths"][12]["spp"]
        self.assertEqual(len(cells[64]["seconds"]), 1)
        self.assertEqual(len(cells[256]["seconds"]), 2)
        self.assertEqual(sorted(result["depths"][24]["spp"]), [256])
        against = result["depths"][24]["against_depth_12"]
        self.assertGreater(against["mean_delta_e"], 0.0)
        self.assertGreater(against["radiance_ratio"], 1.2)

    def test_the_table_lists_every_depth_and_spp_and_the_depth_comparison(self):
        cell = {"relative_noise": 0.1, "seed_pair_delta_e": 2.0, "warm_seconds_mean": 1.5, "peak_mib": 900}
        result = {"base_depth": 12, "depths": {12: {"spp": {64: cell, 256: cell}},
                             24: {"spp": {256: cell}, "against_depth_12": {"mean_delta_e": 0.5, "radiance_ratio": 1.01}}},
                  "export_seconds": 1.0, "export_peak_mib": 500, "cold_seconds": 9.0, "cold_spp": 64, "vram_baseline_mib": 100}
        text = reference_sweep.table(result)
        self.assertEqual(text.count("| 12 | "), 2)
        self.assertIn("| 24 | 256 |", text)
        self.assertIn("| 24 | 0.500 | 1.0100 |", text)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ReferenceBackendOptionsTest(unittest.TestCase):
    def rejected(self, *flags):
        with self.assertRaises(SystemExit) as raised:
            reference_main(["missing.scene.toml", "--poses", "missing.txt", "--out", "unused", *flags])
        self.assertEqual(raised.exception.code, 2)

    def test_a_physical_sky_needs_the_path_traced_backend(self):
        self.rejected("--sky", "hosek-wilkie")

    def test_the_normal_buffer_is_a_bake_output(self):
        self.rejected("--backend", "mitsuba", "--normals")

    def test_spp_and_depth_must_be_positive(self):
        self.rejected("--backend", "mitsuba", "--spp", "0")
        self.rejected("--backend", "mitsuba", "--max-depth", "0")


if __name__ == "__main__":
    unittest.main()
