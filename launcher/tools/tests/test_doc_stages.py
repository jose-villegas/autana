"""The GPU and board doc stages check what they are given before they publish."""
import importlib.util
import hashlib
import json
import tempfile
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


PATH = Path(__file__).resolve().parents[1] / "render/doc_stages.py"
spec = importlib.util.spec_from_file_location("doc_stages", PATH)
stages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stages)
from bake import bake

MEMINFO = Path("/proc/meminfo")
POWERSHELL = Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")


class DocStagesTests(unittest.TestCase):
    def test_capture_intervals_are_required_positive_and_common(self):
        first, second = self.tmp / "a.log", self.tmp / "b.log"
        for text in ("", "pose_every_ms=0", "pose_every_ms=500\npose_every_ms=1000"):
            with self.subTest(text=text):
                first.write_text(text)
                with self.assertRaisesRegex(ValueError, "pose interval"):
                    stages.capture_pose_every_ms([first])
        first.write_text("pose_every_ms=500")
        second.write_text("pose_every_ms=1000")
        with self.assertRaisesRegex(ValueError, "pose interval"):
            stages.capture_pose_every_ms([first, second])

    def test_swapped_pose_rows_keep_their_logged_times(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("CAPTURE pose_every_ms=500\n"
                           "atrium t=500ms clusters=4 tris=5 | both cores: frame 52000us quarter=portrait\n"
                           "atrium t=0ms clusters=4 tris=5 | both cores: frame 51000us quarter=portrait\n")
        expected = [("sponza", 0), ("sponza", 500)]
        self.assertEqual(stages.board_frames([capture], expected, "sponza", "atrium", 500),
                         {("sponza", 0): 51.0, ("sponza", 500): 52.0})

    def test_path_cull_has_its_own_sampling_for_region_visibility(self):
        from types import SimpleNamespace as NS
        import numpy as np
        from r3d import mesh_import, lit_mesh, light, ray_query
        baked = NS(object=NS(name="hall"), renderer=NS(variant=NS(name="full")))
        scene = NS(renderers=[baked])
        geometry = NS(positions=np.zeros((3, 3)), tris=np.array([[0, 1, 2]]),
                      tri_double=np.array([False]), rgb=np.zeros((3, 3)), scale={})
        poses = (224, 224, .5, 1, [[0, 0, 0, 0, 0, -1]])
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(mesh_import, "bake_geometry", return_value=geometry), \
                mock.patch.object(mesh_import, "write_baked"), \
                mock.patch.object(mesh_import, "camera_path_poses", return_value=poses) as sample, \
                mock.patch.object(ray_query, "RayQuery"), \
                mock.patch.object(light, "visible_from_path", return_value=np.array([True])) as visible, \
                mock.patch.object(lit_mesh, "write_lit_mesh"):
            stages.bake_into(Path(directory), scene, "hall", True)
        settings = sample.call_args.args[1]
        self.assertEqual((settings.every_ms, settings.size, settings.samples, settings.margin),
                         (100, (184, 224), 3, 8))
        self.assertEqual(visible.call_args.args[4:], (poses[-1], *poses[:4], 3, 8))

    def test_board_and_smoke_sample_the_fit_with_region_visibility(self):
        from types import SimpleNamespace as NS
        from r3d import import_settings, fitted_variant, mesh_import, reference_render
        fit = NS(size=(8, 6), held_out_every_ms=2000)
        job = NS(renderer=NS(fit=fit, visibility=NS(source="camera_region", rounds=2),
                             variant=NS(name="full")), asset_path=Path("start.mesh"), object=NS(name="hall"))
        scene = NS(renderers=[job])
        args = NS(build_commit=None, capture=[], scene=Path("room.scene.toml"), object="hall")
        poses = (8, 6, .5, 1, [[0, 0, 0, 0, 0, -1]])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "tables").mkdir()
            capture = root / "capture.txt"
            capture.write_text("CAPTURE pose_every_ms=2000\nFRAME COST rendered 8x6\n")
            args.capture = [capture]
            with mock.patch.object(import_settings, "load_scene", return_value=scene), \
                    mock.patch.object(fitted_variant, "placed_variant", return_value=job), \
                    mock.patch.object(mesh_import, "camera_path_poses", return_value=poses) as sample, \
                    mock.patch.object(stages.subprocess, "check_output", return_value=b"commit"), \
                    mock.patch.object(stages.subprocess, "run", return_value=NS(returncode=0)), \
                    mock.patch.object(stages, "board_means", return_value=dict.fromkeys(("room", *stages.VARIANTS), 1)), \
                    mock.patch.object(stages, "board_frames", side_effect=ValueError("stop before cost fitting")):
                with self.assertRaisesRegex(ValueError, "stop before cost fitting"):
                    stages.board(args, root, root)
            sample.assert_called_once_with(scene, fit, fit.held_out_every_ms, either_way_up=False)
            with mock.patch.object(mesh_import, "camera_path_poses", return_value=poses) as sample, \
                    mock.patch.object(stages.shutil, "copyfile"), \
                    mock.patch.object(reference_render, "main") as render:
                stages.smoke_prepare(args.scene, scene, job, root / "inputs")
            sample.assert_called_once_with(scene, fit, 5000, either_way_up=False)
            self.assertEqual(render.call_count, 3)

    def test_viewer_host_build_receives_the_scene(self):
        from r3d.bake_fidelity import build_host
        with mock.patch("r3d.bake_fidelity.subprocess.run") as run:
            run.return_value.stdout = "built viewer\n"
            build_host(Path("scene_viewer.sh"), Path("out"), Path("other.scene.toml"))
        self.assertEqual(run.call_args.args[0], ["sh", "scene_viewer.sh", Path("other.scene.toml").as_posix(),
                                               "--build-only", "-o", "out"])

    def test_measure_worker_renders_the_job_object_and_packs_only_its_scene(self):
        from types import SimpleNamespace as NS
        from r3d import bake_fidelity, appearance_simplify, lit_mesh, cost_model, poses
        import numpy as np
        scene = Path("gallery.scene.toml")
        job = NS(asset_name="gallery.paint", object=NS(name="painting"),
                 renderer=NS(fit=NS(held_out_every_ms=17)))
        with mock.patch.object(poses, "read_poses", return_value=(1, 1, .5, 1, [0, 1])), \
                mock.patch.object(bake_fidelity, "score", return_value=(1, 2, 3, 4, 5)) as score, \
                mock.patch.object(bake_fidelity, "pack_bytes", return_value={}) as pack, \
                mock.patch.object(bake_fidelity, "write_packs"), \
                mock.patch.object(appearance_simplify, "load_views", return_value=([], 1)), \
                mock.patch.object(appearance_simplify, "normal_error", return_value=0), \
                mock.patch.object(appearance_simplify, "start_mesh"), \
                mock.patch.object(lit_mesh, "read_lit_mesh"), \
                mock.patch.object(lit_mesh, "finest_triangles", return_value=([], [], [0])), \
                mock.patch.object(cost_model, "mesh_rows"), \
                mock.patch.object(cost_model, "predict", return_value=np.array([1])):
            stages.measure_worker("paint", job, Path("scratch.mesh"), self.tmp, self.tmp, Path("host"), [], scene)
        fields = score.call_args.args[0].render_args.split()
        self.assertEqual(fields[fields.index("--scene") + 1], "gallery")
        self.assertEqual(fields[fields.index("--object") + 1], "painting")
        pack.assert_called_once_with([scene], ["gallery.paint=scratch.mesh"])

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.tmp = Path(directory.name)

    def test_board_uses_the_callers_scene_and_object(self):
        from types import SimpleNamespace as NS
        from r3d import cost_model, import_settings, mesh_import, fitted_variant
        import numpy as np
        scene_path = Path("gallery.scene.toml")
        job = NS(renderer=NS(fit=NS(held_out_every_ms=1000), visibility=object()))
        scene = NS(renderers=[NS(object=NS(name=name), asset_path=Path(name + ".mesh"))
                              for name in ("painting", "painting_lite")])
        capture = self.tmp / "capture.txt"
        capture.write_text("abcdef123456-diag\nFRAME COST rendered 8x6\nCAPTURE pose_every_ms=1000\n" +
            "".join(f"{name} both cores: mean {value}us\n" for name, value in
                    (("painting", 51000), ("painting_lite", 42000), ("painting_flat", 43000),
                     ("painting_fitted", 44000), ("painting_fitted_full", 45000), ("painting_flat_fitted", 46000))) +
            "painting t= 0s clusters=4 tris=5 | both cores: frame 51000us quarter=portrait\n"
            "painting_lite t= 0s clusters=4 tris=5 | both cores: frame 42000us quarter=portrait\n")
        out, work = self.tmp / "out", self.tmp / "work"
        (out / "tables").mkdir(parents=True)
        work.mkdir()
        weights = self.tmp / "weights.txt"
        weights.write_text("")
        features = np.ones((1, len(cost_model.FEATURES)))
        with mock.patch.object(stages.subprocess, "check_output", return_value=b"abcdef123456"), \
                mock.patch.object(stages.subprocess, "run", return_value=NS(returncode=0)), \
                mock.patch.object(import_settings, "load_scene", return_value=scene), \
                mock.patch.object(fitted_variant, "placed_variant", return_value=job) as placed, \
                mock.patch.object(mesh_import, "camera_path_poses", return_value=(8, 6, .62, 6, [[0]*7])), \
                mock.patch.object(fitted_variant, "poses_text", return_value="poses"), \
                mock.patch.object(cost_model, "mesh_rows", return_value=features) as rows, \
                mock.patch.object(cost_model, "fit", return_value=np.ones(len(cost_model.FEATURES))) as fit, \
                mock.patch.object(stages, "current_stamp", return_value="stamp"), \
                mock.patch.object(stages, "apply_tables"), mock.patch.object(stages, "WEIGHTS", weights), \
                mock.patch.object(stages, "ROOT", self.tmp):
            self.assertEqual(stages.board(NS(scene=scene_path, object="painting", capture=[capture],
                                             build_commit=None, check=False), out, work), 0)
        placed.assert_called_once_with(scene, "painting_fitted")
        self.assertEqual([call.args[0] for call in rows.call_args_list],
                         [Path("painting.mesh"), Path("painting_lite.mesh")])
        self.assertEqual(fit.call_args.args[1], [51.0, 42.0])
        self.assertEqual({path.name for path in (out / "tables").iterdir()},
                         {"gallery-board.md", "gallery-board-model.md"})
        self.assertIn("| gallery | 51.000 |", (out / "tables/gallery-board.md").read_text())

    def test_current_stamp_needs_no_numeric_dependencies(self):
        launcher = self.tmp / "launcher"
        launcher.mkdir()
        (launcher / "m.import.toml").write_text('[source]\npath = "m.obj"\ncredit = "c"\n'
                                               '[output]\nname = "mesh"\n')
        (launcher / "m.obj").write_text("geometry")
        (launcher / "m.mtl").write_text("newmtl m\nmap_Kd texture.png\nmap_d alpha.png\n")
        (launcher / "texture.png").write_bytes(b"texture")
        (launcher / "alpha.png").write_bytes(b"alpha")
        code = """
import importlib.util
from pathlib import Path
import sys
from unittest import mock
sys.modules.update(numpy=None, PIL=None)
spec = importlib.util.spec_from_file_location("doc_stages", sys.argv[1])
stages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stages)
with mock.patch.object(stages, "ROOT", Path(sys.argv[2])), mock.patch.object(
        stages.subprocess, "check_output", return_value=b"launcher/m.import.toml\\0"):
    assert len(stages.current_stamp()) == 64
"""
        result = subprocess.run([sys.executable, "-c", code, str(PATH), str(self.tmp)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_capture_requires_matching_commit(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("other-diag\nlite both cores: mean 42000us\n")
        with self.assertRaisesRegex(ValueError, "build"):
            stages.capture_rows(capture, "abcdef123456", "sponza", "atrium")

    def test_capture_uses_existing_perf_parser(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("abcdef123456-diag\nlite both cores: mean 42000us\n")
        self.assertEqual(stages.capture_rows(capture, "abcdef123456", "sponza", "atrium"), {"lite": 42.0})

    def test_capture_selects_portrait_and_resolves_variant_names(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("abcdef123456-diag\natrium_lite both cores: mean 42000us quarter=portrait\n"
                           "atrium_lite both cores: mean 84000us quarter=landscape\n")
        self.assertEqual(stages.capture_rows(capture, "abcdef123456", "sponza", "atrium"), {"lite": 42.0})

    def test_source_stamp_ignores_doc_refresh_but_not_recipe(self):
        (self.tmp / "launcher").mkdir()
        source = self.tmp / "launcher/recipe.toml"
        source.write_text("steps = 8")
        stamp = stages.source_stamp(self.tmp, [source])
        (self.tmp / "README.md").write_text("refreshed docs")
        self.assertEqual(stages.source_stamp(self.tmp, [source]), stamp)
        source.write_text("steps = 9")
        self.assertNotEqual(stages.source_stamp(self.tmp, [source]), stamp)

    def test_smoke_cannot_publish_or_check(self):
        with self.assertRaisesRegex(ValueError, "smoke"):
            stages.validate_mode(smoke=True, check=True)

    def test_source_stamp_matches_pointer_and_hydrated_content(self):
        source = self.tmp / "source.obj"
        content = b"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
        source.write_bytes(content)
        hydrated = stages.source_stamp(self.tmp, [source])
        checksum = hashlib.sha256(content).hexdigest()
        source.write_bytes(("version https://git-lfs.github.com/spec/v1\n"
                            f"oid sha256:{checksum}\nsize {len(content)}\n").encode())
        self.assertEqual(stages.source_stamp(self.tmp, [source]), hydrated)

    def test_board_rejects_missing_variants(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("abcdef123456-diag\nlite both cores: mean 42000us\n")
        with self.assertRaisesRegex(ValueError, "missing"):
            stages.board_means([capture], "abcdef123456", "sponza", "atrium")

    def test_a_capture_of_all_six_bake_entities_yields_every_board_variant(self):
        names = ("painting", "painting_flat", "painting_lite", "painting_fitted", "painting_fitted_full", "painting_flat_fitted")
        capture = self.tmp / "capture.txt"
        lines = [f"{name} both cores: mean {1000 * (k + 1)}us" for k, name in enumerate(names)]
        capture.write_text("\n".join(["abcdef123456-diag", *lines]) + "\n")
        means = stages.board_means([capture], "abcdef123456", "gallery", "painting")
        self.assertEqual(set(means), {"gallery", *stages.VARIANTS})
        self.assertEqual(means, {"gallery": 1.0, "flat": 2.0, "lite": 3.0, "fitted": 4.0, "fitted-full": 5.0,
                                 "flat-fitted": 6.0})

    def test_a_capture_without_the_flat_fitted_entity_is_refused(self):
        names = ("painting", "painting_flat", "painting_lite", "painting_fitted", "painting_fitted_full")
        capture = self.tmp / "capture.txt"
        lines = [f"{name} both cores: mean 42000us" for name in names]
        capture.write_text("\n".join(["abcdef123456-diag", *lines]) + "\n")
        with self.assertRaisesRegex(ValueError, "flat-fitted"):
            stages.board_means([capture], "abcdef123456", "gallery", "painting")

    def test_board_half_second_pose_rows(self):
        capture = self.tmp / "capture.txt"
        capture.write_text('CAPTURE period_ms=1000 pose_every_ms=500\n'
                           'atrium t=    0s clusters=4 tris=5 | both cores: frame 51000us quarter=portrait\n'
                           'atrium t=  500ms clusters=4 tris=5 | both cores: frame 52000us quarter=portrait\n'
                           'atrium_lite t=    0s clusters=4 tris=5 | both cores: frame 42000us quarter=portrait\n'
                           'atrium_lite t=  500ms clusters=4 tris=5 | both cores: frame 43000us quarter=portrait\n')
        expected = [(name, time) for name in ('sponza', 'lite') for time in (0, 500)]
        self.assertEqual(stages.board_frames([capture], expected, 'sponza', 'atrium', 500),
                         {('sponza', 0): 51.0, ('sponza', 500): 52.0,
                          ('lite', 0): 42.0, ('lite', 500): 43.0})

    def test_board_pose_rows_require_complete_unique_samples(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("atrium t=    0s clusters= 4 tris=5 | both cores: frame 51000us quarter=portrait\n"
                           "atrium_lite t=    0s clusters= 4 tris=5 | both cores: frame 42000us quarter=portrait\n")
        expected = [("sponza", 0), ("lite", 0)]
        self.assertEqual(stages.board_frames([capture], expected, "sponza", "atrium", 500), {("sponza", 0): 51.0, ("lite", 0): 42.0})
        with self.assertRaisesRegex(ValueError, "exactly once"):
            stages.board_frames([capture], [*expected, ("sponza", 5)], "sponza", "atrium", 500)
        capture.write_text(capture.read_text() + capture.read_text().splitlines()[0] + "\n")
        with self.assertRaisesRegex(ValueError, "exactly once"):
            stages.board_frames([capture], expected, "sponza", "atrium", 500)

    def test_gpu_check_requires_saved_full_run(self):
        with self.assertRaisesRegex(ValueError, "no full run"):
            stages.check_gpu(None, self.tmp)

    def test_board_rejects_a_different_render_size(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("=== atrium FRAME COST (10 tris, 20 verts, 1 clusters, rendered 368x448) ===\n")
        with self.assertRaisesRegex(ValueError, "render size"):
            stages.capture_viewport(capture, (184, 224))

    def test_gpu_check_rejects_missing_saved_artifact(self):
        (self.tmp / "source.json").write_text(json.dumps({"source": bake.digest({}),
                                                          "files": {"tables/score.md": "0" * 64}}))
        with mock.patch.object(stages, "gpu_inputs", return_value={}):
            with self.assertRaisesRegex(ValueError, "missing or altered"):
                stages.check_gpu(mock.Mock(), self.tmp)

    def guard_with(self, wsl_kib, host_bytes, error=None):
        """Run memory_guard with WSL's MemAvailable and the host's AvailableBytes faked."""
        read_text, exists = Path.read_text, Path.exists

        def fake_read_text(path, *args, **kwargs):
            if path == MEMINFO:
                return f"MemAvailable: {wsl_kib} kB\n"
            return read_text(path, *args, **kwargs)

        def fake_exists(path):
            return True if path == POWERSHELL else exists(path)

        with mock.patch.object(Path, "read_text", fake_read_text), \
                mock.patch.object(Path, "exists", fake_exists), \
                mock.patch.object(stages.subprocess, "check_output", return_value=f"{host_bytes}\r\n".encode(), side_effect=error):
            stages.memory_guard()

    def test_memory_guard_checks_limiting_windows_host(self):
        with self.assertRaisesRegex(ValueError, "Windows host is short by 1.00 GiB"):
            self.guard_with(wsl_kib=16 * 1024 ** 2, host_bytes=1 * 1024 ** 3)

    def test_memory_guard_checks_limiting_wsl(self):
        with self.assertRaisesRegex(ValueError, "WSL is short by 2.00 GiB"):
            self.guard_with(wsl_kib=4 * 1024 ** 2, host_bytes=16 * 1024 ** 3)

    def test_memory_guard_passes_with_enough_on_both_sides(self):
        self.guard_with(wsl_kib=6 * 1024 ** 2, host_bytes=2 * 1024 ** 3)

    def test_memory_guard_reports_interop_failure(self):
        with self.assertRaisesRegex(ValueError, "Windows host memory query failed.*Invalid argument"):
            self.guard_with(wsl_kib=8 * 1024 ** 2, host_bytes=0, error=OSError("Invalid argument"))

    def test_memory_guard_reports_unparseable_host_memory(self):
        with self.assertRaisesRegex(ValueError, "Windows host memory query failed.*not a byte count"):
            self.guard_with(wsl_kib=8 * 1024 ** 2, host_bytes="Invalid argument")

    def test_smoke_preserves_full_gpu_results(self):
        result = self.tmp / "out/gpu/measurements.json"
        result.parent.mkdir(parents=True)
        result.write_text("full run")
        with mock.patch.object(stages, "RESULTS", self.tmp), mock.patch.object(stages, "gpu", lambda *args: 0):
            self.assertEqual(stages.main(["--stage", "gpu", "--smoke", "--scene", "other.scene.toml", "--object", "hall"]), 0)
        self.assertEqual(result.read_text(), "full run")


class RayTracingPreflightTests(unittest.TestCase):
    def test_a_runner_that_cannot_load_the_llvm_backend_is_refused_with_the_fix(self):
        failed = subprocess.CompletedProcess([], 1, "", "ImportError: libatomic.so.1: cannot open shared object file")
        with mock.patch.object(stages.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(ValueError, r"libatomic\.so\.1.*install libatomic1 and libLLVM"):
                stages.require_ray_tracing()

    def test_a_runner_without_an_llvm_backend_says_so(self):
        absent = subprocess.CompletedProcess([], 3, "", "")
        with mock.patch.object(stages.subprocess, "run", return_value=absent):
            with self.assertRaisesRegex(ValueError, "no LLVM backend"):
                stages.require_ray_tracing()

    def test_a_runner_with_the_backend_passes(self):
        with mock.patch.object(stages.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")):
            self.assertIsNone(stages.require_ray_tracing())

    def test_the_check_asks_drjit_for_the_llvm_backend(self):
        self.assertIn("has_backend(dr.JitBackend.LLVM)", stages.RAY_TRACING_CHECK)


class FakeKeeper:
    """Stands in for stages.Keeper: records each request, (kind, args, future), and leaves the future to the test."""

    def __init__(self):
        self.requests = []

    def request(self, kind, *args):
        from concurrent.futures import Future
        self.requests.append((kind, args, Future()))
        return self.requests[-1][2]

    def start(self, job, scene):
        return self.request("start", job, scene)

    def references(self, job, scene):
        return self.request("references", job, scene)

    def fit(self, point, point_dir, scene_path, scene, job, inputs):
        return self.request("fit", point, point_dir, scene_path, scene, job, inputs)

    def bake(self, scene, baked_name, path_cull):
        return self.request("bake", scene, baked_name, path_cull)

    def of(self, kind):
        return [(args, future) for made, args, future in self.requests if made == kind]


class PrepareSchedulingTests(unittest.TestCase):
    def test_a_variant_is_ready_once_its_start_and_references_are(self):
        keeper = FakeKeeper()
        seen = []
        jobs = ['lite-job', 'full-job']
        with mock.patch.object(stages, 'link_inputs') as link:
            ready = stages.prepare_variants(keeper, [('scene', job, Path('work') / job) for job in jobs],
                                            lambda index, job: seen.append((index, job)))
            self.assertEqual([kind for kind, _, _ in keeper.requests], ['start', 'references'] * 2,
                             "every start and reference set is asked for before any variant is consumed")
            (_, start), (_, references) = keeper.of('start')[0], keeper.of('references')[0]
            start.set_result(stages.Kept(Path('start'), ['start step']))
            self.assertEqual(seen, [])
            references.set_result(stages.Kept(Path('references'), []))
        self.assertEqual(seen, [(0, 'lite-job')])
        link.assert_called_once_with(Path('work/lite-job'), Path('start'), Path('references'))
        self.assertEqual(ready[0].result(), ['start step'])
        self.assertFalse(ready[1].done())

    def test_a_failed_start_reaches_the_consumer_without_waiting_for_its_references(self):
        keeper = FakeKeeper()
        callback = mock.Mock()
        ready = stages.prepare_variants(keeper, [('scene', 'job', Path('work'))], callback)
        keeper.of('start')[0][1].set_exception(RuntimeError('start failure'))
        with self.assertRaisesRegex(RuntimeError, 'start failure'):
            ready[0].result()
        callback.assert_not_called()

    def test_memory_guard_is_only_called_at_stage_start(self):
        import ast
        tree = ast.parse(PATH.read_text())
        callers = [node.name for node in tree.body if isinstance(node, ast.FunctionDef)
                   for call in ast.walk(node) if isinstance(call, ast.Call)
                   and isinstance(call.func, ast.Name) and call.func.id == 'memory_guard']
        self.assertEqual(callers, ['gpu'])


class LightStudyTests(unittest.TestCase):
    SCENE = stages.ROOT / "launcher/demo/sponza/sponza.scene.toml"

    def test_each_look_scene_loads_and_fits_the_showcase(self):
        from r3d.fitted_variant import placed_variant
        from r3d.import_settings import load_scene
        showcase = load_scene(self.SCENE).renderers[0].object.name
        with tempfile.TemporaryDirectory() as directory:
            for look in ("smooth", "direct", "ao-occluded"):
                with self.subTest(look=look):
                    path = stages.write_look_scene(self.SCENE, look, Path(directory) / look)
                    scene = load_scene(path)
                    self.assertEqual(scene.renderers[0].object.name, showcase)
                    self.assertTrue(placed_variant(scene, showcase).renderer.fit)
                    self.assertEqual(scene.bake.ao is not None, look == "ao-occluded")

    def test_a_fit_is_asked_for_only_after_its_prepare_and_its_failure_reaches_the_sheet(self):
        keeper = FakeKeeper()
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(stages, 'link_inputs'):
            studies, ready, fits = stages.submit_light_fits(keeper, self.SCENE, Path(directory))
            self.assertEqual(len(keeper.of('start')), len(studies))
            self.assertEqual(len(keeper.of('references')), len(studies))
            self.assertEqual((keeper.of('fit'), fits), ([], {}))
            keeper.of('start')[0][1].set_result(stages.Kept(Path('start'), []))
            keeper.of('references')[0][1].set_result(stages.Kept(Path('references'), []))
            look, path, scene, job, inputs = studies[0]
            self.assertEqual(list(fits), [look])
            self.assertEqual(keeper.of('fit')[0][0], ({}, inputs.parent / "fit", path, scene, job, inputs))
            keeper.of('start')[1][1].set_exception(RuntimeError("start failure"))
            with self.assertRaisesRegex(RuntimeError, "start failure"):
                ready[1].result()
            self.assertNotIn(studies[1][0], fits)


class SmokeAdmissionTests(unittest.TestCase):
    def test_smoke_submits_measured_fit_reservation(self):
        from types import SimpleNamespace
        from r3d import fitted_variant, import_settings
        from r3d.fit import stage
        from r3d.process_budget import FIT_BYTES
        job = SimpleNamespace(renderer=SimpleNamespace(fit=SimpleNamespace(held_out_every_ms=1)))
        executor = mock.Mock()
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(import_settings, 'load_scene', return_value=object()), \
                mock.patch.object(fitted_variant, 'placed_variant', return_value=job), \
                mock.patch.object(stages, 'gpu_inputs', return_value={}):
            stages._gpu(SimpleNamespace(smoke=True, scene=Path("other.scene.toml"), object="hall"), Path(directory), Path(directory), executor)
        fit_call = executor.submit.call_args_list[1]
        self.assertIs(fit_call.args[0], stage.fit_point)
        self.assertEqual(fit_call.kwargs['estimates'], FIT_BYTES)


class FullGpuSchedulingTests(unittest.TestCase):
    def test_fit_arguments_resume_and_order_with_reverse_completion(self):
        from concurrent.futures import Future
        from types import SimpleNamespace as NS
        from r3d import fitted_variant as fitted, import_settings, bake_fidelity, cost_model
        from r3d.process_budget import MEASURE_BYTES
        jobs = [NS(renderer=NS(fit=NS(held_out_every_ms=1000, normal_weight=.3, budget=6000),
                               variant=NS(triangles=10000))) for _ in range(3)]
        baked_variants = {"hall_lite": "lite", "hall": "full", "hall_flat": "flat"}
        scene = NS(renderers=[NS(object=NS(name=name), renderer=NS(variant=NS(name=variant)))
                              for name, variant in baked_variants.items()])
        consumed, measured, completed = [], {}, []
        step = dict(wall_s=1, triangles_in=4, triangles_out=3, peak_ram_bytes=None, peak_vram_bytes=None)

        class Keeper(FakeKeeper):
            """Starts, references and bakes are made at once; the fits finish in reverse, when the first bake is
            asked for."""
            def request(self, kind, *args):
                future = super().request(kind, *args)
                if kind in ("start", "references"):
                    future.set_result(stages.Kept(Path(kind), [dict(step="prepare fixture", **step)]))
                if kind == "bake":
                    for (point, point_dir, *_), fit in reversed(self.of("fit")):
                        if not fit.done():
                            completed.append(point_dir.name)
                            fit.set_result(stages.Kept(Path("kept") / point_dir.name,
                                                       [dict(step="fit fixture", **{**step, "triangles_in": 3, "triangles_out": 2})]))
                    future.set_result(stages.Kept(Path("kept") / args[1], [dict(step="bake fixture", **{**step, "triangles_in": 5, "triangles_out": 4})]))
                return future

        keeper = Keeper()

        class Executor:
            def submit(self, function, *args, **kwargs):
                if function is not stages.measure_worker or kwargs != {"estimates": MEASURE_BYTES, "priority": True}:
                    raise AssertionError((function, kwargs))
                label, _job, mesh = args[:3]
                consumed.append(label)
                measured[label] = mesh
                future = Future()
                future.set_result(((label, 1, '1.5', '2.5', '3.5', '4.5', '99.5'), Path(f'{label}.avi'),
                                   {'triangles': 1, 'mean_delta_e': 0., 'predicted_ms': 0.}))
                return future

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            out = root / 'out'
            (out / 'tables').mkdir(parents=True)
            (out / 'render/gpu').mkdir(parents=True)
            work = root / 'work/full-' / bake.digest({})
            resume = work / 'budget-4000-cost-0/result.json'
            resume.parent.mkdir(parents=True)
            resume.write_text(json.dumps({'budget': 4000, 'cost_weight': 0., 'triangles': 1,
                                          'mean_delta_e': 0., 'predicted_ms': 0.}))
            before = resume.read_bytes()
            with mock.patch.object(import_settings, 'load_scene', return_value=scene), \
                    mock.patch.object(fitted, 'placed_variant', side_effect=jobs) as placed, \
                    mock.patch.object(bake_fidelity, 'build_host', return_value='host') as build_host, \
                    mock.patch.object(cost_model, 'load', return_value=([],)), \
                    mock.patch.object(stages, 'gpu_inputs', return_value={}), \
                    mock.patch.object(stages, 'Keeper', return_value=keeper), \
                    mock.patch.object(stages, 'link_inputs') as link, \
                    mock.patch.object(stages, 'run') as run, mock.patch.object(stages, 'apply_tables'), \
                    mock.patch.object(stages, 'sync_images') as sync, \
                    mock.patch.object(fitted, 'plot_pareto'), \
                    mock.patch.object(stages, 'machine_table', return_value='machine fixture'), \
                    mock.patch.object(stages, 'submit_light_fits', return_value='light fits') as light, \
                    mock.patch.object(stages, 'light_sheets') as sheets:
                stages._gpu(NS(smoke=False, scene=Path("other.scene.toml"), object="hall"), out, root / 'work', Executor())
            light.assert_called_once_with(keeper, Path('other.scene.toml'), work)
            self.assertEqual(sheets.call_args.args[0], 'light fits')
            build_host.assert_called_once_with(stages.HOST_SCRIPT, work / 'host', Path('other.scene.toml'))
            self.assertEqual(placed.call_args_list, [mock.call(mock.ANY, "hall_fitted"),
                                                    mock.call(mock.ANY, "hall_fitted_full"),
                                                    mock.call(mock.ANY, "hall_flat_fitted")])
            self.assertEqual([args[1:] for args, _ in keeper.of('bake')],
                             [("hall_lite", False), ("hall", True), ("hall_flat", False)])
            self.assertEqual([call.args for call in link.call_args_list],
                             [(work / f"inputs-{prefix}", Path("start"), Path("references")) for prefix in ("lite", "full", "flat")])
            self.assertEqual({path.name for path in (out / "tables").iterdir()},
                             {"other-normal.md", "other-budget.md", "other-gpu.md", "other-flat-fit.md", "bake-steps.md"})
            table = (out / 'tables/bake-steps.md').read_text()
            for prefix in ('lite', 'full', 'flat'):
                self.assertIn(f"| {prefix}-GI-bake | bake fixture | 100% | 5 | 4 |", table)
                self.assertIn(f"| {prefix}-fit-start | prepare fixture | 100% | 4 | 3 |", table)
                self.assertIn(f"| {prefix}-GI-fit | fit fixture | 100% | 3 | 2 |", table)
            self.assertNotIn("Wall s", table)
            self.assertIn("![Bake steps](../images/render/gpu/bake-steps.png)", table)
            self.assertTrue((out / 'render/gpu/bake-steps.png').is_file())
            sync.assert_called_once_with(out / 'render/gpu', stages.ROOT / 'docs/images/render/gpu', check=False)
            expected_rows = ['lite-GI-bake', 'lite-GI-fit', 'normal-0', 'normal-0.1', 'normal-0.3',
                             'budget-4000-cost-0.1', 'budget-6000-cost-0', 'budget-6000-cost-0.1',
                             'full-GI-bake', 'full-path-culled', 'full-GI-fit', 'flat-GI-bake', 'flat-GI-fit']
            self.assertEqual(consumed, expected_rows)
            self.assertEqual({label: measured[label] for label in ('lite-GI-bake', 'full-path-culled', 'lite-GI-fit',
                                                                   'normal-0', 'normal-0.3', 'budget-6000-cost-0')},
                             {'lite-GI-bake': Path('kept/hall_lite/lite.mesh'),
                              'full-path-culled': Path('kept/hall/culled.mesh'),
                              'lite-GI-fit': Path('kept/fit-lite/fitted.mesh'),
                              'normal-0': Path('kept/normal-0/fitted.mesh'),
                              'normal-0.3': Path('kept/fit-lite/fitted.mesh'),
                              'budget-6000-cost-0': Path('kept/fit-lite/fitted.mesh')},
                             "a kept result is measured from its cache folder; the recipe's own point is its fit")
            measurements = json.loads((out / 'measurements.json').read_text())
            self.assertEqual([row[0] for row in measurements['rows']], expected_rows)
            self.assertEqual([(row['budget'], row['cost_weight']) for row in measurements['sweep']],
                             [(4000, 0.), (4000, .1), (6000, 0.), (6000, .1), (6000, 0.)])
            self.assertEqual(resume.read_bytes(), before)
            fits = keeper.of('fit')
            self.assertEqual(completed, [args[1].name for args, _ in reversed(fits)])
            self.assertCountEqual([(point_dir.name, point.get('budget'), point.get('cost_weight', 0.),
                                    job.renderer.fit.normal_weight, inputs.name)
                                   for (point, point_dir, _path, _scene, job, inputs), _ in fits], [
                ('fit-lite', None, 0., .3, 'inputs-lite'), ('fit-full', None, 0., .3, 'inputs-full'),
                ('fit-flat', None, 0., .3, 'inputs-flat'),
                ('normal-0', None, 0., 0., 'inputs-lite'), ('normal-0.1', None, 0., .1, 'inputs-lite'),
                ('budget-4000-cost-0.1', 4000, .1, .3, 'inputs-lite'),
                ('budget-6000-cost-0.1', 6000, .1, .3, 'inputs-lite')])
            flat_sheet = [call.args[0] for call in run.call_args_list if call.args[1].name == 'sheet-flat.log']
            self.assertEqual(len(flat_sheet), 1)
            command = [str(word) for word in flat_sheet[0]]
            columns = [(command[i + 1], command[i + 2]) for i, word in enumerate(command) if word == '--bake']
            self.assertEqual(columns, [('flat bake', 'flat-GI-bake.avi'), ('lite fit', 'lite-GI-fit.avi'),
                                       ('flat fit', 'flat-GI-fit.avi')])
            table = (out / 'tables/other-flat-fit.md').read_text().splitlines()
            self.assertEqual([line.split(' | ')[0].strip('| ') for line in table[2:5]],
                             ['flat-GI-bake', 'lite-GI-fit', 'flat-GI-fit'], "the sheet's column order")
            self.assertNotIn('Predicted', table[0], "the cost model cannot price flat shading")
            cells = [line.count('|') for line in table[:5]]
            self.assertEqual(len(set(cells)), 1, "a row has more cells than the header")
            self.assertNotIn('99.5', ' '.join(table), "a predicted time reached the flat table")
            self.assertEqual(sum(line.startswith('| ') for line in table), 4, "a header and exactly three rows")


class KeeperTests(unittest.TestCase):
    """The keeper on the demo scene's real keys, with a scratch cache and an executor that only records."""
    SCENE = stages.ROOT / "launcher/demo/sponza/sponza.scene.toml"

    def setUp(self):
        from concurrent.futures import Future
        from r3d.import_settings import load_scene
        from r3d.fitted_variant import placed_variant
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.submitted = []

        def submit(function, *args, **options):
            self.submitted.append((function, args, options))
            return Future()
        self.keeper = stages.Keeper(mock.Mock(submit=mock.Mock(side_effect=submit)), self.root / "cache")
        self.scene = load_scene(self.SCENE)
        self.jobs = [placed_variant(self.scene, "atrium" + entry.name) for entry in stages.FITTED]
        self.inputs = self.root / "inputs"
        self.inputs.mkdir()
        (self.inputs / f"{self.jobs[0].renderer.variant.name}.mesh").write_bytes(b"start")

    def tearDown(self):
        self.directory.cleanup()

    def fit_folder(self, point=None, job=None, scene=None):
        future = self.keeper.fit(point or {}, self.root / "point", self.SCENE, scene or self.scene, job or self.jobs[0],
                                 self.inputs)
        return next(folder for folder, made in self.keeper.made.items() if made is future)

    def test_variants_that_share_their_inputs_share_one_reference_set_from_bakes_folder(self):
        from bake.produce import REFERENCES
        for job in self.jobs:
            self.keeper.references(job, self.scene)
        self.assertEqual(len(self.submitted), 1, "lite, full and flat read one reference set")
        function, (key, *_rest, cache), _options = self.submitted[0]
        self.assertIs(function, stages.kept_references)
        self.assertEqual((key, cache), (self.keeper.keys(self.jobs[0], self.scene)["reference"], self.root / "cache"))
        self.assertIs(self.keeper.references(self.jobs[1], self.scene),
                      self.keeper.made[self.root / "cache" / REFERENCES / key])

    def test_a_done_folder_is_not_made_again_and_replays_its_steps(self):
        from bake.produce import DONE
        future = self.keeper.start(self.jobs[0], self.scene)
        folder = self.submitted[0][1][0]
        folder.mkdir(parents=True)
        (folder / DONE).write_text("done\n")
        (folder / stages.STEPS).write_text(json.dumps([{"step": "carve"}]))
        later = stages.Keeper(self.keeper.executor, self.root / "cache")
        kept = later.start(self.jobs[0], self.scene).result()
        self.assertEqual(kept, stages.Kept(folder, [{"step": "carve"}]))
        self.assertEqual(len(self.submitted), 1, "the next run makes nothing")
        self.assertFalse(future.done())

    def test_each_sweep_point_and_look_fits_under_its_own_key(self):
        import copy
        from r3d.fitted_variant import placed_variant
        from r3d.import_settings import load_scene
        folders = {"recipe": self.fit_folder()}
        normal = copy.deepcopy(self.jobs[0])
        normal.renderer.fit.normal_weight = 0.1
        folders["normal"] = self.fit_folder(job=normal)
        folders["budget"] = self.fit_folder({"budget": 4000, "cost_weight": 0.0})
        folders["cost"] = self.fit_folder({"budget": 4000, "cost_weight": 0.1})
        look = load_scene(stages.write_look_scene(self.SCENE, "direct", self.root / "look"))
        folders["look"] = self.fit_folder(job=placed_variant(look, self.jobs[0].renderer.variant.name), scene=look)
        self.assertEqual(len(set(folders.values())), len(folders), folders)
        self.assertEqual(self.fit_folder({"budget": self.jobs[0].renderer.fit.budget, "cost_weight": 0.0}),
                         folders["recipe"], "the recipe's budget given as a point is the recipe's fit")
        (self.inputs / f"{self.jobs[0].renderer.variant.name}.mesh").write_bytes(b"other start")
        self.assertNotEqual(self.fit_folder(), folders["recipe"], "a fit counts its start's bytes")

    def test_a_cost_weighted_fit_counts_the_cost_model(self):
        weighted = {"budget": 4000, "cost_weight": 0.1}
        before = self.fit_folder(weighted)
        unweighted = self.fit_folder({"budget": 4000, "cost_weight": 0.0})
        weights = self.root / "weights.txt"
        weights.write_text("feature other\n")
        with mock.patch.object(stages, "WEIGHTS", weights):
            self.assertNotEqual(self.fit_folder(weighted), before)
            self.assertEqual(self.fit_folder({"budget": 4000, "cost_weight": 0.0}), unweighted)

    def test_the_path_cull_bake_is_kept_apart_from_the_plain_bake(self):
        self.keeper.bake(self.scene, "atrium", False)
        self.keeper.bake(self.scene, "atrium", True)
        self.keeper.bake(self.scene, "atrium" + stages.FITTED[0].baked, False)
        self.assertEqual(len({args[0] for _function, args, _options in self.submitted}), 3)

    def test_the_code_that_makes_a_result_is_in_its_key(self):
        self.keeper.start(self.jobs[0], self.scene)
        before = self.submitted[-1][1][0]
        with mock.patch.object(stages, "code_digest", return_value="changed"):
            self.keeper.start(self.jobs[0], self.scene)
        self.assertNotEqual(self.submitted[-1][1][0], before)

    def test_inputs_link_the_reference_set_and_the_start(self):
        references, start = self.root / "references", self.root / "start"
        (references / "reference").mkdir(parents=True)
        (references / "train.txt").write_text("poses")
        start.mkdir()
        (start / "lite.mesh").write_bytes(b"mesh")
        (start / stages.STEPS).write_text("[]")
        inputs = self.root / "linked"
        for _ in range(2):
            stages.link_inputs(inputs, start, references)
        self.assertEqual(sorted(path.name for path in inputs.iterdir()), ["lite.mesh", "reference", "train.txt"])
        self.assertEqual((inputs / "lite.mesh").read_bytes(), b"mesh")
        self.assertTrue((inputs / "reference").is_dir())


@unittest.skipUnless(sys.platform == "linux", "the GPU stage and its fingerprint run on Linux")
class GpuInputsTests(unittest.TestCase):
    def test_the_fingerprint_covers_what_the_stage_runs_without_numeric_dependencies(self):
        """A hosted runner with no numpy or Pillow takes it; it names the scripts the stage starts, the
        files its code names, what the host renderer compiles and the scene's locked bakes."""
        code = """
import importlib.util, json, sys
from pathlib import Path
sys.modules.update(numpy=None, PIL=None)
spec = importlib.util.spec_from_file_location("doc_stages", sys.argv[1])
stages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stages)
from bake import bake
inputs = stages.gpu_inputs(stages.ROOT / "launcher/demo/sponza/sponza.scene.toml", "atrium")
assert bake.digest(inputs) == bake.digest(
    stages.gpu_inputs(stages.ROOT / "launcher/demo/sponza/sponza.scene.toml", "atrium"))
print(json.dumps(inputs))
"""
        result = subprocess.run([sys.executable, "-c", code, str(PATH)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        inputs = json.loads(result.stdout)
        for name in ("code launcher/tools/render/doc_stages.py", "code launcher/tools/render/render_compare.py",
                     "code launcher/tools/r3d/fitted_variant.py", "file launcher/tools/r3d/board_cost_weights.txt",
                     "host launcher/main/render/raster.c", "host launcher/tools/render/scene_viewer.c",
                     "native launcher/main/display/display.h",
                     "bake sponza.atrium.bundle", "bake sponza.atrium_fitted.bundle"):
            self.assertIn(name, inputs)
        self.assertNotIn("code launcher/tools/bake/bake.py", inputs)
        self.assertEqual([name for name in inputs if "/apps/" in name], [])


if __name__ == "__main__":
    unittest.main()
