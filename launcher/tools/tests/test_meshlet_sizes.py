"""Meshlet settings, capture identities and geometry-derived report cells."""
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path[:0] = [str(TOOLS), str(TOOLS / "render")]
from r3d.import_settings import load_import_settings
from test_r3d_import import write_import

class MeshletSettingsTests(unittest.TestCase):
    def test_default_and_explicit_limits(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write_import(directory)
            self.assertEqual(load_import_settings(path).meshlet_triangles, 32)
            for size in (16, 64):
                path = write_import(directory, body=f"[geometry]\nmeshlet_triangles = {size}\n")
                self.assertEqual(load_import_settings(path).meshlet_triangles, size)

    def test_rebake_reads_import_limit_and_default_is_byte_identical(self):
        import numpy as np
        from r3d import rebake
        from r3d.lit_mesh import read_lit_mesh, write_lit_mesh
        from test_r3d_bake import sheet
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_lit_mesh(root, "tiny", *sheet(20))
            entry = root / "tiny.mesh"
            original = entry.read_bytes()
            settings = write_import(root)
            rebake.main([str(entry), "--import", str(settings)])
            self.assertEqual(entry.read_bytes(), original)
            for size in (16, 64):
                settings = write_import(root, body=f"[geometry]\nmeshlet_triangles = {size}\n")
                rebake.main([str(entry), "--import", str(settings)])
                mesh = read_lit_mesh(entry)
                self.assertLessEqual(max(cluster[3] for cluster in mesh.clusters), size)
                self.assertTrue(any(cluster[3] > size // 2 for cluster in mesh.clusters))

    def test_default_keeps_committed_fit_recipe_hashes(self):
        from r3d.import_settings import load_scene
        from r3d.fitted_variant import recipe_digest
        for path in (TOOLS.parents[1] / "launcher/demo").glob("*/*.scene.toml"):
            scene = load_scene(path)
            for job in scene.renderers:
                if job.renderer.fit:
                    with self.subTest(mesh=job.asset_name):
                        self.assertEqual(recipe_digest(job, scene), job.renderer.fit.recipe_sha256)

class MeshletReportTests(unittest.TestCase):
    def capture(self, root, row, build="abcdef123456", pack="a", suite=True):
        path = root / f"meshlets-{row}-board.log"
        label = "cull_off" if row == "cull-off" else "atrium"
        stages = "ms/frame avg/worst: r3d.cull 1/2 r3d.transform 2/3 r3d.draw 3/4 | total 6\n"
        if suite:
            path.write_text(f"=== {label} FRAME COST (1 tris, 3 verts, 1 clusters, rendered 184x224) ===\n"
                            f"MESHLET_CAPTURE build_id={build}-diag pack_crc32={pack * 8} size={32 if row == 'cull-off' else row} cull={int(row != 'cull-off')} period_ms=10000 pose_every_ms=5000\n"
                            f"{label} t=    0s clusters=1 tris=1 | both cores: frame 7000us\n" + stages +
                            f"{label} t=    5s clusters=1 tris=1 | both cores: frame 9000us\n" + stages +
                            f"{label} both cores: mean 8000us (125 fps before present), worst 9000us\n"
                            "=== other FRAME COST (1 tris, 3 verts, 1 clusters, rendered 184x224) ===\n"
                            "ms/frame avg/worst: r3d.cull 99/99 r3d.transform 99/99 r3d.draw 99/99 | total 297\n"
                            "other both cores: mean 297000us (3 fps before present), worst 297000us\n")
        else:
            path.write_text(f"MESHLET_CAPTURE build_id={build}-dev pack_sha256={pack * 64} size={row} cull=1\n" + stages)
        return path

    def test_monitor_capture_is_rejected_and_suite_accepted(self):
        import meshlet_sizes
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for row, pack in (("cull-off", "b"), ("16", "a"), ("32", "b"), ("64", "c")):
                self.capture(root, row, pack=pack, suite=False)
            with self.assertRaises(ValueError):
                meshlet_sizes.capture_means(root, "atrium")
            for row, pack in (("cull-off", "b"), ("16", "a"), ("32", "b"), ("64", "c")):
                self.capture(root, row, pack=pack)
            self.assertEqual(meshlet_sizes.capture_means(root, "atrium")["32"], [1, 2, 3, 8])

    def test_order_missing_capture_and_identity(self):
        import meshlet_sizes
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for row, pack in (("cull-off", "b"), ("16", "a"), ("32", "b"), ("64", "c")):
                self.capture(root, row, pack=pack)
            with self.assertRaisesRegex(ValueError, "pack"):
                meshlet_sizes.capture_means(root, "atrium", {"16": "d" * 8, "32": "b" * 8, "64": "c" * 8})
            values = meshlet_sizes.capture_means(root, "atrium")
            self.assertEqual(list(values), ["cull-off", "16", "32", "64"])
            self.assertEqual(values["16"], [1, 2, 3, 8])
            path = self.capture(root, "64", build="fedcba123456", pack="c")
            self.assertEqual(meshlet_sizes.capture_means(root, "atrium")["64"], [1, 2, 3, 8])
            path.write_text(path.read_text() + "BUILD_ID=abcdef123456-diag\n")
            with self.assertRaisesRegex(ValueError, "build"):
                meshlet_sizes.capture_means(root, "atrium")
            (root / "meshlets-64-board.log").unlink()
            with self.assertRaises(FileNotFoundError):
                meshlet_sizes.capture_means(root, "atrium")

    def test_commands_hold_one_lock_per_row(self):
        import meshlet_capture
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            meshlet_capture.commands(pathlib.Path("scratch"), dict(sizes=[16,32,64], suite="run_sponza_perf_suite"))
        lines = output.getvalue().splitlines()
        self.assertEqual(len(lines), 4)
        for row, line in zip(("cull-off", "16", "32", "64"), lines):
            self.assertIn(f"--project scratch/tree/{row} suite run_sponza_perf_suite --flash --out", line)
            self.assertTrue(line.endswith(f"meshlets-{row}-board.log"))

    def test_suite_requires_shared_poses_and_render_size(self):
        import meshlet_sizes
        from doc_stages import capture_viewport
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for row, pack in (("cull-off", "b"), ("16", "a"), ("32", "b"), ("64", "c")):
                path = self.capture(root, row, pack=pack)
                capture_viewport(path, (184, 224))
            self.assertEqual(meshlet_sizes.capture_means(root, "atrium", expected_poses=[0, 5000])["32"], [1, 2, 3, 8])
            path.write_text(path.read_text().replace("t=    5s", "t=    6s"))
            with self.assertRaisesRegex(ValueError, "poses"):
                meshlet_sizes.capture_means(root, "atrium")
            path = self.capture(root, "64", pack="c")
            path.write_text(path.read_text().replace("rendered 184x224", "rendered 368x448"))
            with self.assertRaisesRegex(ValueError, "render size"):
                capture_viewport(path, (184, 224))

    def test_host_columns_from_fixture(self):
        import numpy as np
        import meshlet_sizes
        from r3d.lit_mesh import write_lit_mesh
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            mesh = write_lit_mesh(root, "tiny", np.array([[0,0,-2], [1,0,-2], [0,1,-2]], float),
                                  np.full((3,3), 128), np.array([[0,1,2]]), np.array([1]))
            poses = root / "poses.txt"
            poses.write_text("size 8 6\nlens 1 0.1\npose 0 0 0 0 0 -1\npose 100 0 0 0 0 -1\n")
            self.assertEqual(meshlet_sizes.host_columns([root / "tiny.mesh"], poses), (1, 3.0, 0.5))
            self.assertEqual(meshlet_sizes.host_columns([root / "tiny.mesh"], poses, cull=False), (1, 3.0, 1.0))

if __name__ == "__main__":
    unittest.main()
