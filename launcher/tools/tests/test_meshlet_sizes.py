"""Meshlet settings, capture identities and geometry-derived report cells."""
import contextlib
import io
import pathlib
import shutil
import subprocess
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

    def test_cli_meshlet_limit_overrides_import(self):
        from r3d import rebake
        from r3d.lit_mesh import read_lit_mesh, write_lit_mesh
        from test_r3d_bake import sheet
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_lit_mesh(root, "tiny", *sheet(20))
            settings = write_import(root, body="[geometry]\nmeshlet_triangles = 64\n")
            rebake.main([str(root / "tiny.mesh"), "--import", str(settings), "--meshlet-triangles", "16"])
            counts = [cluster[3] for cluster in read_lit_mesh(root / "tiny.mesh").clusters]
            self.assertLessEqual(max(counts), 16)
            self.assertTrue(any(count > 8 for count in counts))

    def test_import_writer_uses_the_jobs_meshlet_limit(self):
        import numpy as np
        from types import SimpleNamespace
        from r3d import mesh_import
        from r3d.lit_mesh import read_lit_mesh
        from test_r3d_bake import sheet
        positions, rgb, tris, double = sheet(20)
        geometry = SimpleNamespace(positions=positions, rgb=rgb, tris=tris, tri_double=double, scale={})
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for size in (16, 64):
                job = SimpleNamespace(renderer=SimpleNamespace(face_samples=None),
                                      settings=SimpleNamespace(meshlet_triangles=size))
                mesh_import.write_baked(job, None, root, "tiny", geometry)
                counts = [cluster[3] for cluster in read_lit_mesh(root / "tiny.mesh").clusters]
                self.assertLessEqual(max(counts), size)
                self.assertTrue(any(count > size // 2 for count in counts))

    def test_meshlet_bounds(self):
        from r3d.import_settings import SettingsError
        with tempfile.TemporaryDirectory() as directory:
            for size in (4, 256):
                path = write_import(directory, body=f"[geometry]\nmeshlet_triangles = {size}\n")
                self.assertEqual(load_import_settings(path).meshlet_triangles, size)
            for size in (3, 257):
                path = write_import(directory, body=f"[geometry]\nmeshlet_triangles = {size}\n")
                with self.assertRaisesRegex(SettingsError, "meshlet_triangles"):
                    load_import_settings(path)

    def test_meshlet_limit_is_part_of_the_bake_key(self):
        import copy
        from bake.bake import mesh_recipe
        from r3d.import_settings import load_scene
        scene = load_scene(TOOLS.parents[1] / "launcher/demo/sponza/sponza.scene.toml")
        job = scene.renderers[0]
        changed = copy.deepcopy(job)
        changed.settings.meshlet_triangles = job.settings.meshlet_triangles // 2
        self.assertNotEqual(mesh_recipe(job, scene, {}), mesh_recipe(changed, scene, {}))

class MeshletReportTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("gcc"), "needs gcc")
    def test_assertion_after_cull_switch_restores_the_prior_setting(self):
        source = (TOOLS.parents[1] / "launcher/main/apps/render_lab/tests/suite_sponza_perf.c").read_text()
        body = source[source.index("static int32_t saved_cull;"):source.index("/* The full bake with motion")]
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            probe = work / "probe.c"
            probe.write_text('''#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "services/tune.h"
TUNE_OWNER(render);
TUNE(render, cull, 0, 0, 1);
static jmp_buf aborted;
static void (*cleanup)(void);
static void suite_set_test_cleanup(void (*fn)(void)) { cleanup = fn; }
#define TEST_ASSERT_NOT_NULL(p) do { if ((p) == 0) longjmp(aborted, 1); } while (0)
#define TEST_PASS() ((void)0)
#define ESP_LOGI(...) ((void)0)
enum { SPONZA_BAKE_COUNT = 1, SPONZA_BAKE_FULL = 0 };
static const int meshes[1];
static const char* const sponza_bakes[] = {"renderer"};
typedef struct { const int* mesh; void* placement; } r3d_instance_t;
static void open_the_meshes(void) {}
static void report_frame_cost(const char* label, const char* object_name, const r3d_instance_t* instance, void* a, void* b) {
    /* An allocation assertion aborts the measured pass after culling is switched off. */
    TEST_ASSERT_NOT_NULL(strcmp(label, "cull_off") ? instance : 0);
}
''' + body + '''
int main(void) {
    for (int initial = 0; initial <= 1; initial++) {
        tune_handle_line(initial ? "SET render.cull 1" : "SET render.cull 0", cull_reply);
        cleanup = 0;
        if (setjmp(aborted) == 0) {
            test_sponza_frame_cost_along_the_flythrough();
            assert(0);
        }
        assert(cull == 0);
        assert(cleanup != 0);
        cleanup();
        assert(cull == initial);
    }
    return 0;
}
''')
            binary = work / "probe.exe"
            main = TOOLS.parents[1] / "launcher/main"
            built = subprocess.run(["gcc", "-std=gnu11", "-I", str(main), "-I", str(TOOLS.parents[1] / "launcher/test/stubs"),
                                    str(probe), str(main / "services/tune.c"), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)

    def test_incomplete_capture_is_rejected(self):
        import meshlet_sizes
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self.capture_rows(root)
            path = root / "meshlets-64-board.log"
            path.write_text(path.read_text().split(":test_frame_cost:PASS")[0])
            with self.assertRaisesRegex(ValueError, "complete"):
                meshlet_sizes.capture_means(root, "atrium")

    def capture(self, root, row, build="abcdef123456", pack="a", suite=True):
        path = root / f"meshlets-{row}-board.log"
        label = "atrium"
        stages = "ms/frame avg/worst: r3d.cull 1/2 r3d.transform 2/3 r3d.draw 3/4 | total 6\n"
        if suite:
            path.write_text(f"=== {label} FRAME COST (1 tris, 3 verts, 1 clusters, rendered 184x224) ===\n"
                            f"CAPTURE build_id={build}-diag pack_crc32={pack * 8} object=atrium size={row} cull=1 period_ms=10000 pose_every_ms=5000\n"
                            f"{label} t=    0s clusters=1 tris=1 | both cores: frame 7000us\n" + stages +
                            f"{label} t=    5s clusters=1 tris=1 | both cores: frame 9000us\n" + stages +
                            f"{label} both cores: mean 8000us (125 fps before present), worst 9000us\n"
                            "=== other FRAME COST (2 tris, 6 verts, 1 clusters, rendered 184x224) ===\n"
                            "ms/frame avg/worst: r3d.cull 99/99 r3d.transform 99/99 r3d.draw 99/99 | total 297\n"
                            "other both cores: mean 297000us (3 fps before present), worst 297000us\n")
        else:
            path.write_text(f"MESHLET_CAPTURE build_id={build}-dev pack_sha256={pack * 64} size={row} cull=1\n" + stages)
        if suite:
            text = path.read_text()
            if row == "32":
                block = text.split("=== other")[0]
                text += block.replace("atrium FRAME", "cull_off FRAME").replace("atrium t=", "cull_off t=").replace("atrium both", "cull_off both").replace("cull=1", "cull=0")
            path.write_text(text + ":test_frame_cost:PASS\nRUNSUITE_COMPLETE name=run_perf found=1 selected=1 unmatched=0\n")
        return path

    def capture_rows(self, root):
        for row, pack in (("16", "a"), ("32", "b"), ("64", "c")):
            path = self.capture(root, row, pack=pack)
        return path

    def test_monitor_capture_is_rejected_and_suite_accepted(self):
        import meshlet_sizes
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for row, pack in (("16", "a"), ("32", "b"), ("64", "c")):
                self.capture(root, row, pack=pack, suite=False)
            with self.assertRaisesRegex(ValueError, "complete"):
                meshlet_sizes.capture_means(root, "atrium")
            self.capture_rows(root)
            self.assertEqual(meshlet_sizes.capture_means(root, "atrium")["32"], [1, 2, 3, 8])

    def test_order_missing_capture_and_identity(self):
        import meshlet_sizes
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self.capture_rows(root)
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

    def test_row_installs_one_size_and_prints_one_locked_command(self):
        import meshlet_capture
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "bakes/16").mkdir(parents=True)
            (root / "tree/assets").mkdir(parents=True)
            (root / "bakes/16/mesh.mesh").write_bytes(b"size16")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                meshlet_capture.install_row(root, dict(sizes=[16,32,64], suite="run_perf", meshes=["assets/mesh.mesh"]), 16)
            self.assertEqual((root / "tree/assets/mesh.mesh").read_bytes(), b"size16")
            lines = output.getvalue().splitlines()
            self.assertEqual(len(lines), 1)
            self.assertIn("suite run_perf --flash --out", lines[0])
            self.assertTrue(lines[0].endswith("meshlets-16-board.log"))

    def test_suite_requires_shared_poses_and_render_size(self):
        import meshlet_sizes
        from doc_stages import capture_viewport
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = self.capture_rows(root)
            for capture in root.glob("*.log"):
                capture_viewport(capture, (184, 224))
            self.assertEqual(meshlet_sizes.capture_means(root, "atrium", expected_poses=[0, 5000])["32"], [1, 2, 3, 8])
            path.write_text(path.read_text().replace("t=    5s", "t=    6s"))
            with self.assertRaisesRegex(ValueError, "poses"):
                meshlet_sizes.capture_means(root, "atrium")
            path = self.capture(root, "64", pack="c")
            path.write_text(path.read_text().replace("rendered 184x224", "rendered 368x448"))
            with self.assertRaisesRegex(ValueError, "render size"):
                capture_viewport(path, (184, 224))

    def test_invalid_capture_contracts(self):
        import meshlet_sizes
        cases = (("size=64", "size=16", "wrong size"),
                 ("cull=1", "cull=0", "cull setting"),
                 ("cccccccc", "aaaaaaaa", "distinct size pack"),
                 (":test_frame_cost:PASS", ":test_frame_cost:FAIL", "failed suite"),
                 ("object=atrium", "object=other", "another object"))
        for old, new, reason in cases:
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                path = self.capture_rows(root)
                path.write_text(path.read_text().replace(old, new))
                with self.assertRaisesRegex(ValueError, reason):
                    meshlet_sizes.capture_means(root, "atrium")

    def test_expected_pose_and_geometry_mismatches(self):
        import meshlet_sizes
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self.capture_rows(root)
            with self.assertRaisesRegex(ValueError, "host camera samples"):
                meshlet_sizes.capture_means(root, "atrium", expected_poses=[0, 10000])
            with self.assertRaisesRegex(ValueError, "header geometry"):
                meshlet_sizes.capture_means(root, "atrium", expected_geometry={row:(2,3,1) for row in meshlet_sizes.ROWS})
            path = root / "meshlets-32-board.log"
            path.write_text(path.read_text().replace("object=atrium size=32 cull=0", "object=other size=32 cull=0"))
            with self.assertRaisesRegex(ValueError, "another object"):
                meshlet_sizes.capture_means(root, "atrium")

    def test_completion_verdict_alone_is_required(self):
        import meshlet_sizes
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = self.capture_rows(root)
            path.write_text(path.read_text().split("RUNSUITE_COMPLETE")[0])
            with self.assertRaisesRegex(ValueError, "complete"):
                meshlet_sizes.capture_means(root, "atrium")

    def test_current_identity_and_object_are_required(self):
        import meshlet_sizes
        for old, new in (("CAPTURE build_id", "MESHLET_CAPTURE build_id"), ("object=atrium ", "")):
            with self.subTest(identity=new), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                path = self.capture_rows(root)
                path.write_text(path.read_text().replace(old, new))
                with self.assertRaisesRegex(ValueError, "identity"):
                    meshlet_sizes.capture_means(root, "atrium")

    def test_second_identity_with_another_build_is_rejected(self):
        import meshlet_sizes
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = self.capture_rows(root)
            identity = next(line for line in path.read_text().splitlines() if line.startswith("CAPTURE "))
            path.write_text(path.read_text() + identity.replace("abcdef123456", "fedcba123456") + "\n")
            with self.assertRaisesRegex(ValueError, "mismatched build"):
                meshlet_sizes.capture_means(root, "atrium")

    def test_capture_preparation_guards(self):
        import meshlet_capture
        from unittest import mock
        (TOOLS / "results").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TOOLS / "results") as directory:
            work = pathlib.Path(directory)
            (work / "tree").mkdir()
            with mock.patch.object(meshlet_capture, "tracked_files", return_value=[]), \
                    self.assertRaisesRegex(ValueError, "already exists"):
                meshlet_capture.scratch_tree(work)
            with self.assertRaisesRegex(ValueError, "not prepared"):
                meshlet_capture.install_row(work, dict(sizes=[32]), 16)
            for args, reason in ((["row", "--work", str(work.parent.parent)], "inside"),
                                 (["prepare", "--work", str(work)], "prepare needs"),
                                 (["prepare", "--work", str(work), "--scene", "scene", "--suite", "suite", "--sizes", "16"], "prepare needs"),
                                 (["prepare", "--work", str(work), "--scene", "scene", "--suite", "suite", "--sizes", "32", "32"], "prepare needs"),
                                 (["row", "--work", str(work)], "row needs")):
                with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()) as error:
                    self.assertEqual(meshlet_capture.main(args), 2)
                    self.assertIn(reason, error.getvalue())

    def test_meshlets_check_exit_code_on_fixture(self):
        import doc_stages
        import meshlet_sizes
        import generated_blocks
        from unittest import mock
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            doc = root / "docs/render/Render-Pipeline.md"
            doc.parent.mkdir(parents=True)
            doc.write_text(f"<!-- generated: meshlet-sizes sha256={generated_blocks.digest(chr(10) + 'fixture' + chr(10))} -->\nfixture\n<!-- /generated: meshlet-sizes -->\n")
            args = ["--stage", "meshlets", "--check", "--scene", "fixture.scene.toml", "--object", "atrium"]
            with mock.patch.object(doc_stages, "ROOT", root), mock.patch.object(doc_stages, "RESULTS", root / "results"), \
                    mock.patch.object(meshlet_sizes, "table", return_value="fixture\n"), \
                    mock.patch.object(generated_blocks, "tracked_files", return_value=["docs/render/Render-Pipeline.md"]):
                self.assertEqual(doc_stages.main(args), 0)
                doc.write_text(doc.read_text().replace("fixture", "stale"))
                self.assertEqual(doc_stages.main(args), 1)
                self.assertIn("stale", doc.read_text())

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
            self.assertEqual(meshlet_sizes.host_columns([root / "tiny.mesh"], poses), ((1, 3, 1), 0.5))
            self.assertEqual(meshlet_sizes.host_columns([root / "tiny.mesh"], poses, cull=False), ((1, 3, 1), 1.0))

if __name__ == "__main__":
    unittest.main()
