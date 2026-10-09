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
    def capture(self, root, row, build="abcdef123456", pack="a"):
        path = root / f"meshlets-{row}-board.log"
        path.write_text(f"MESHLET_CAPTURE build_id={build}-dev pack_sha256={pack * 64} size={32 if row == 'cull-off' else row} cull={int(row != 'cull-off')}\n"
                        "ms/frame avg/worst: r3d.cull 1/2 r3d.transform 2/3 r3d.draw 3/4 | total 8\n")
        return path

    def test_order_missing_capture_and_identity(self):
        import meshlet_sizes
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for row, pack in (("cull-off", "b"), ("16", "a"), ("32", "b"), ("64", "c")):
                self.capture(root, row, pack=pack)
            with self.assertRaisesRegex(ValueError, "pack"):
                meshlet_sizes.capture_means(root, {"16": "d" * 64, "32": "b" * 64, "64": "c" * 64})
            values = meshlet_sizes.capture_means(root)
            self.assertEqual(list(values), ["cull-off", "16", "32", "64"])
            self.assertEqual(values["16"], [1, 2, 3, 8])
            self.capture(root, "64", build="fedcba123456", pack="c")
            with self.assertRaisesRegex(ValueError, "build"):
                meshlet_sizes.capture_means(root)
            (root / "meshlets-64-board.log").unlink()
            with self.assertRaises(FileNotFoundError):
                meshlet_sizes.capture_means(root)

    def test_commands_open_the_requested_scene(self):
        import meshlet_capture
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            meshlet_capture.commands(pathlib.Path("scratch"), dict(sizes=[16,32,64], open_app="Viewer",
                                     scene_command=["render", "scene", "reference"]))
        lines = output.getvalue().splitlines()
        self.assertEqual(lines.count("autana open Viewer"), 4)
        self.assertEqual(lines.count("autana render scene reference"), 4)

    def test_total_from_frame_cost_owner(self):
        from r3d.dynres_report import frame_cost_means
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = self.capture(root, "16")
            self.assertEqual(dict(frame_cost_means(path, include_total=True))["frame.total"], 8.0)

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
