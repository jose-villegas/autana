"""Pipeline costs use recorded stages and injected machine probes."""
import pathlib
import sys
import tempfile
import unittest
import threading
from unittest.mock import patch

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "render"))
from r3d import process_budget
import doc_stages


class PipelineMeasuresTests(unittest.TestCase):
    def test_peak_is_maximum_even_when_probe_falls(self):
        probe = iter([(100, 200), (10, 20)])
        recorder = process_budget.StepRecorder(probe=lambda gpu: next(probe), interval=None)
        with recorder.step("fit", 12):
            pass
        self.assertEqual(recorder.rows[0]["peak_ram_bytes"], 100)
        self.assertEqual(recorder.rows[0]["peak_vram_bytes"], 200)

    def test_raising_body_records_wall_time_and_propagates(self):
        recorder = process_budget.StepRecorder(clock=iter([1, 4]).__next__, probe=lambda gpu: (0, None), interval=None)
        with self.assertRaisesRegex(RuntimeError, "body"):
            with recorder.step("fit", 12):
                raise RuntimeError("body")
        self.assertEqual(recorder.rows[0]["wall_s"], 3)

    def test_default_poll_captures_mid_step_peak_and_joins(self):
        entered, released = threading.Event(), threading.Event()
        threads = []
        def probe(gpu):
            if threading.current_thread() is not threading.main_thread():
                threads.append(threading.current_thread())
                entered.set()
                if not released.wait(5):
                    raise RuntimeError("probe not released")
                return 100, None
            return 1, None
        recorder = process_budget.StepRecorder(probe=probe)
        with recorder.step("fit", 12):
            try:
                self.assertTrue(entered.wait(5))
            finally:
                released.set()
        self.assertEqual(recorder.rows[0]["peak_ram_bytes"], 100)
        self.assertTrue(threads)
        self.assertTrue(all(not thread.is_alive() for thread in threads))

    def test_default_gpu_poll_never_launches_a_subprocess(self):
        sampled = threading.Event()
        def rss(pid, field):
            if threading.current_thread() is not threading.main_thread():
                sampled.set()
            return 10
        recorder = process_budget.StepRecorder()
        with patch.object(process_budget, "status_bytes", side_effect=rss), \
                patch.object(process_budget, "gpu_resident_bytes", return_value={}) as gpu:
            with recorder.step("fit", 12, gpu=True):
                self.assertTrue(sampled.wait(5))
        self.assertEqual(gpu.call_count, 2)

    def test_null_recorder_starts_no_thread_or_probe(self):
        with patch.object(process_budget.threading, "Thread") as thread, \
                patch.object(process_budget, "step_memory") as probe:
            with process_budget.NULL_RECORDER.step("source load", 0) as step:
                step["triangles_out"] = 12
        thread.assert_not_called()
        probe.assert_not_called()

    def test_repeated_name_raises_and_keeps_first_input(self):
        recorder = process_budget.StepRecorder(probe=lambda gpu: (0, None), interval=None)
        with recorder.step("light", 12):
            pass
        with self.assertRaises(ValueError):
            with recorder.step("light", 99):
                pass
        self.assertEqual(recorder.rows[0]["triangles_in"], 12)

    def test_missing_memory_is_not_available(self):
        row = dict(step="fit", wall_s=1, triangles_in=2, triangles_out=1,
                   peak_ram_bytes=None, peak_vram_bytes=None)
        self.assertIn("| not available | not available |", doc_stages.bake_steps_table([("fixture", [row])]))

    def test_machine_parsing_and_field_order(self):
        probes = doc_stages.machine_probes("model name : Fixture CPU\nphysical id : 0\ncore id : 0\n",
                                          "MemTotal: 8388608 kB\nMemAvailable: 1024 kB\n",
                                          "Fixture GPU, 4096, 123\n", dict(threads=2, CUDA="12", OS="host",
                                          Python="3", Mitsuba="1", PyTorch="2"))
        self.assertEqual(probes['RAM'], '8.0 GiB')
        self.assertEqual(probes['Cores/threads'], '1/2')
        self.assertEqual(probes['GPU'], 'Fixture GPU')
        self.assertEqual(probes['VRAM'], '4096 MiB')
        self.assertEqual(probes['Driver'], '123')
        self.assertEqual([line.split('|')[1].strip() for line in doc_stages.machine_table(probes).splitlines()[2:]],
                         list(doc_stages.MACHINE_FIELDS))
    def test_importer_counts_real_stage_boundaries_without_gpu(self):
        import numpy as np
        from types import SimpleNamespace as NS
        from r3d import mesh_import, lit_mesh
        source = NS(p=np.array([[0., 0., 0.], [1., 0., 0.], [0., 1., 0.], [1., 1., 0.]]),
                    tri_v=np.array([[0, 1, 2], [1, 3, 2], [0, 1, 2], [0, 1, 2]]), tri_t=np.zeros((4, 3), dtype=int),
                    tri_m=np.array([0, 1, 0, 0]), uv=[], textures={}, names=["surface", "foliage"])
        settings = NS(seed=5, position_scale=None, alpha_keep=.5, thin=NS(material="foliage", keep=0),
                      simplify=None, double_sided=[])
        job = NS(settings=settings, renderer=NS(visibility=object(), face_samples=None), bake=None)
        recorder = process_budget.StepRecorder(probe=lambda gpu: (100, None), interval=None)
        with patch.object(mesh_import, "load_source", return_value=source), \
                patch.object(mesh_import, "drop_masked", return_value=(source.tri_v[:3], source.tri_t[:3], source.tri_m[:3])), \
                patch.object(mesh_import, "RayQuery"), \
                patch.object(mesh_import, "visible_triangles", return_value=np.array([True, True, False])), \
                patch.object(mesh_import, "shade_unlit", side_effect=lambda src, mat, p, t: (p, np.zeros_like(p), t)):
            geometry = mesh_import.bake_geometry(job, None, recorder=recorder)
        mesh = NS(tris=geometry.tris)
        with tempfile.TemporaryDirectory() as directory, patch.object(lit_mesh, "bake_lit_mesh", return_value=mesh), \
                patch.object(lit_mesh, "mesh_blob", return_value=b"mesh"):
            mesh_import.write_baked(job, None, pathlib.Path(directory), "fixture", geometry, recorder=recorder)
        self.assertEqual([row["step"] for row in recorder.rows],
                         ["source load", "alpha mask", "visibility", "thin", "light", "simplify", "meshlets", "write"])
        self.assertEqual([(row["triangles_in"], row["triangles_out"]) for row in recorder.rows],
                         [(0, 4), (4, 3), (3, 2), (2, 1), (1, 1), (1, 1), (1, 1), (1, 1)])
        self.assertTrue(all(row["peak_vram_bytes"] is None for row in recorder.rows))
        self.assertFalse(hasattr(mesh, "measurements"))

        recorder = process_budget.StepRecorder(probe=lambda gpu: (100, None), interval=None)
        job.renderer.face_samples = (1, 1, 1, 1)
        with tempfile.TemporaryDirectory() as directory, patch.object(mesh_import, "flat_colours", return_value=[]), \
                patch.object(lit_mesh, "bake_lit_mesh", return_value=mesh), patch.object(lit_mesh, "mesh_blob", return_value=b"mesh"):
            mesh_import.write_baked(job, None, pathlib.Path(directory), "flat", geometry, recorder=recorder)
        self.assertEqual([row['step'] for row in recorder.rows], ['face colours', 'meshlets', 'write'])

    def test_measured_fit_records_actual_input_output_triangles(self):
        from types import SimpleNamespace as NS
        from r3d import fitted_variant, lit_mesh
        job = NS(renderer=NS(variant=NS(name="fixture")))
        recorder = process_budget.StepRecorder(probe=lambda gpu: (100, 200), interval=None)
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(lit_mesh, "read_lit_mesh", side_effect=[NS(tris=[0]*12), NS(tris=[0]*8)]), \
                patch.object(fitted_variant, "fit", return_value=pathlib.Path(directory) / "fit.mesh"):
            result = fitted_variant.fit_point({}, pathlib.Path(directory) / "work", None, None, job,
                                              pathlib.Path(directory), recorder=recorder)
        self.assertEqual([(row["step"], row["triangles_in"], row["triangles_out"])
                          for row in recorder.rows], [("fit", 12, 8)])
        self.assertEqual(recorder.rows[0]["peak_vram_bytes"], 200)

    def test_step_recorder_accumulates_once_and_preserves_counts(self):
        recorder = process_budget.StepRecorder(clock=iter([0, 1, 2, 4, 5, 8]).__next__,
                                               probe=lambda gpu: (12, 34 if gpu else None), interval=None)
        with recorder.step("source load", 0) as step:
            step["triangles_out"] = 10
        with recorder.step("light", 10, gpu=True) as step:
            step["triangles_out"] = 10
        with self.assertRaises(ValueError):
            with recorder.step("light", 99, gpu=True):
                pass
        self.assertEqual([row["step"] for row in recorder.rows], ["source load", "light"])
        self.assertEqual([(row["triangles_in"], row["triangles_out"]) for row in recorder.rows], [(0, 10), (10, 10)])
        self.assertEqual([row["wall_s"] for row in recorder.rows], [1, 2])
        self.assertEqual([row["peak_ram_bytes"] for row in recorder.rows], [12, 12])
        self.assertEqual([row["peak_vram_bytes"] for row in recorder.rows], [None, 34])

    def test_machine_table_uses_injected_probes_and_step_columns(self):
        probes = {name: name + " fixture" for name in doc_stages.MACHINE_FIELDS}
        table = doc_stages.machine_table(probes)
        for name in probes:
            self.assertIn(f"| {name} | {name} fixture |", table)
        rows = [{"step": "fit", "wall_s": 2.5, "triangles_in": 12, "triangles_out": 8,
                 "peak_ram_bytes": 1048576, "peak_vram_bytes": 2097152}]
        table = doc_stages.bake_steps_table([("variant", rows)])
        self.assertEqual(table.splitlines()[0],
                         "| Variant | Step | Wall s | Triangles in | Triangles out | Peak RAM MiB | Peak VRAM MiB |")
        self.assertIn("| variant | fit | 2.500 | 12 | 8 | 1.0 | 2.0 |", table)



if __name__ == "__main__":
    unittest.main()
