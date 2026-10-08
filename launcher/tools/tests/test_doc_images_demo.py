"""doc_images.sh's bake copies keep their import file; physical_scene.py
drops only the scene's indirect table and occlusion."""

import pathlib
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest
from unittest import mock
from types import SimpleNamespace

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
DEMO_SCENE = ROOT / "launcher/demo/sponza/sponza.scene.toml"
DEMO_IMPORT = ROOT / "launcher/demo/sponza/sponza.import.toml"
APP_SCRIPT = ROOT / "launcher/tools/render/doc_images_demo.sh"


@unittest.skipUnless(shutil.which("sh"), "needs sh")
class DocImageFailureTest(unittest.TestCase):
    def test_scene_poses_equal_track_host_for_the_video_frame_count(self):
        from anim import track_host
        from anim_probe import write_camera_clip
        from test_r3d_import import HEAD, renderer, sun_object, write_import
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            clip = write_camera_clip(work, name="orbit", reach=2.0, degrees=90.0)
            write_import(work)
            scene = work / "gallery.scene.toml"
            scene.write_text(HEAD + sun_object() + renderer(name="painting_fitted", extra=
                'bake = true\nvisibility = { source = "camera_path", every_ms = 100, size = [8, 6] }\n') +
                '[[objects]]\nname = "view"\n[objects.camera]\nhalf_fov_short_tan = 0.62\nnear_z = 6\n'
                'path = { animation = "orbit.anim.toml", node = "camera" }\n')
            source = APP_SCRIPT.read_text()
            block = source[source.index('run "$PYTHON" - "$SCENE"'):source.index("\nPYPOSES") + len("\nPYPOSES")]
            for frames in (2, 5):
                with self.subTest(frames=frames):
                    script = (ROOT / "scripts/lib/run.sh").read_text() + \
                        '\nSCENE=$1\nW=$2\nPYTHON=$3\nFULL=painting\nFIDELITY_FRAMES=$4\nFIDELITY_DT=250\n' + block
                    done = subprocess.run(["sh", "-c", script, "poses", scene.as_posix(), work.as_posix(),
                                           pathlib.Path(sys.executable).as_posix(), str(frames)],
                                          cwd=ROOT, capture_output=True, text=True, timeout=60)
                    self.assertEqual(done.returncode, 0, done.stderr)
                    expected = track_host.sample(clip, ["--every", 250, "--until", (frames + 1) * 250,
                                                       "--poses", "camera", 8, 6, "0.62", "6"])
                    self.assertEqual((work / "fidelity-poses.txt").read_bytes(), expected.encode())

    def test_tables_use_the_callers_scene_and_object(self):
        sys.path.insert(0, str(ROOT / "launcher/tools/render"))
        import doc_tables
        import numpy as np
        values = (1, 2, 3, 4, 5)
        def scores(path):
            self.assertNotIn("atrium", path.name)
            if path.name.endswith("_lite-compare.log"):
                self.assertEqual(path.name, "painting_lite-compare.log")
            return {name: values for name in ("frames mean", "two bounces", "direct light only",
                    "intensity 1", "intensity 2", "intensity 3", "albedo boost 2",
                    "intensity 1 (own reference)", "intensity 2 (own reference)",
                    "intensity 3 (own reference)", "albedo boost 2 (own reference)")}
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            (work / "sampling").mkdir()
            (work / "sampling/table.md").write_text("sampling\n")
            pair = SimpleNamespace(mean_delta_e=1, p95_delta_e=2, ssim_luma=3,
                                   edge_delta_e=4, interior_delta_e=5)
            with mock.patch.object(doc_tables, "scores", side_effect=scores) as read, \
                    mock.patch.object(doc_tables, "read_video", return_value=(None, [np.zeros((2, 2, 3), dtype=np.uint8)])), \
                    mock.patch.object(doc_tables, "reference_video", return_value=(None, pair)):
                doc_tables.write_tables(work, work / "tables", "gallery", "painting")
            self.assertIn(mock.call(work / "painting_lite-compare.log"), read.call_args_list)
            self.assertEqual({path.name for path in (work / "tables").iterdir()},
                             {f"gallery-{suffix}.md" for suffix in
                              ("fidelity", "flat-smooth", "flat-sampling", "indirect", "indirect-look")})

    def test_driver_supplies_all_four_demo_arguments(self):
        source = (ROOT / "launcher/tools/render/render_doc_images.sh").read_text()
        assignments = [line for line in source.splitlines() if line.startswith(("DEMO_SCENE=", "DEMO_OBJECT="))]
        calls = [line for line in source.splitlines() if line.startswith('run bash "$TOOLS_DIR/doc_images_demo.sh"')]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            stub = root / "doc_images_demo.sh"
            stub.write_text('printf "%s\\n" "$@" > argv\n')
            script = (ROOT / "scripts/lib/run.sh").read_text() + '\nTOOLS_DIR=.\nOUT=out\nWORK=work\n'
            script += "\n".join(assignments + calls)
            done = subprocess.run(["sh", "-c", script], cwd=root, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual((root / "argv").read_text().splitlines(),
                             ["out", "work/demo", DEMO_SCENE.relative_to(ROOT).as_posix(), "atrium"])

    def test_orphans_require_a_demo_prefix_and_a_matching_template(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            tools = root / "launcher/tools/render"
            tools.mkdir(parents=True)
            for name in ("render_doc_images.sh", "doc_images_demo.sh", "doc_import_examples.py"):
                shutil.copyfile(ROOT / "launcher/tools/render" / name, tools / name)
            helper = root / "scripts/lib/run.sh"
            helper.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / "scripts/lib/run.sh", helper)
            demo = root / "launcher/demo/gallery/gallery.scene.toml"
            demo.parent.mkdir(parents=True)
            demo.touch()
            images = root / "docs/images/render"
            images.mkdir(parents=True)
            for stem in ("gallery-full", "gallery-missing", "unknown-full"):
                (images / f"{stem}.gif").touch()
            done = subprocess.run(["sh", str(tools / "render_doc_images.sh"), "--orphans"],
                                  cwd=root, capture_output=True, text=True)
            self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
            self.assertNotIn("orphan render/gallery-full.gif", done.stdout)
            self.assertIn("orphan render/gallery-missing.gif", done.stdout)
            self.assertIn("orphan render/unknown-full.gif", done.stdout)

    def test_scratch_bake_packs_the_replacement_and_renders_its_object(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            tools = root / "launcher/tools/r3d"
            tools.mkdir(parents=True)
            (tools / "mesh_import.py").write_text("exit 0\n")
            (tools / "build_pack.py").write_text('printf "%s\n" "$@" > pack.argv\n')
            host = root / "host.sh"
            host.write_text('printf "%s\n" "$@" > render.argv\nprintf "%s\n" "$AUTANA_ASSET_DIR" > assets.env\n')
            host.chmod(0o755)
            (root / "scratch").mkdir()
            source = APP_SCRIPT.read_text()
            function = "bake_and_render() {" + source.split("bake_and_render() {", 1)[1].split("\n}", 1)[0] + "\n}\n"
            script = (ROOT / "scripts/lib/run.sh").read_text() + \
                'PYTHON=sh\nR3D_PYTHON=sh\nHOST=./host.sh\nSCENE=gallery.scene.toml\nID=gallery\nDEMO=.\n' + \
                function + 'bake_and_render scratch gallery.paint painting\n'
            done = subprocess.run(["sh", "-c", script], cwd=root, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual((root / "pack.argv").read_text().splitlines(),
                             ["-o", "scratch/assets", "gallery.scene.toml", "--replace", "gallery.paint=scratch/gallery.paint.mesh"])
            args = (root / "render.argv").read_text().splitlines()
            self.assertEqual(args[args.index("--scene") + 1], "gallery")
            self.assertEqual(args[args.index("--object") + 1], "painting")
            self.assertEqual((root / "assets.env").read_text().strip(), "scratch/assets")

    def test_variant_scene_keeps_its_import_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            meshes = root / DEMO_SCENE.parent.relative_to(ROOT)
            shutil.copytree(DEMO_SCENE.parent, meshes,
                            ignore=shutil.ignore_patterns("*.mesh", "*.c", "*.h"))
            importer = root / "launcher/tools/r3d/mesh_import.py"
            importer.parent.mkdir(parents=True)
            importer.write_text(
                f'test -f "$(dirname "$1")/{DEMO_IMPORT.name}" || exit 7\n'
                'echo import_dependency_ready\nexit 8\n', encoding="utf-8")
            source = APP_SCRIPT.read_text(encoding="utf-8")
            helper = (ROOT / "scripts/lib/run.sh").read_text(encoding="utf-8")
            functions = source.split("bake_and_render() {", 1)[1].split("\nvariant_bake smooth", 1)[0]
            functions = "\n".join(line for line in functions.splitlines() if "physical_scene.py" not in line)
            script = root / "stage.sh"
            script.write_text(
                'set -e\nW=work\nID=sponza\nFULL=atrium\nR3D_PYTHON=sh\nmkdir -p work\n'
                f'DEMO={DEMO_SCENE.parent.relative_to(ROOT).as_posix()}\n'
                f'cp "$DEMO/{DEMO_SCENE.name}" work/physical.scene.toml\n'
                + helper + 'bake_and_render() {' + functions + "\nvariant_bake direct none ''\n", encoding="utf-8")
            result = subprocess.run(["sh", script.as_posix()], cwd=root, capture_output=True, text=True)
            log = (root / "work/indirect-direct/bake.log").read_text(encoding="utf-8")
            self.assertEqual(result.returncode, 8, result.stderr + log)
            self.assertIn("import_dependency_ready", log)


class PhysicalSceneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import importlib.util
        path = ROOT / "launcher/tools/render/physical_scene.py"
        spec = importlib.util.spec_from_file_location("physical_scene", path)
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def test_the_committed_scene_loses_its_indirect_table_and_occlusion_and_nothing_else(self):
        text = DEMO_SCENE.read_text(encoding="utf-8")
        committed, physical = tomllib.loads(text), tomllib.loads(self.module.physical_scene(text))
        self.assertIn("indirect", committed)
        self.assertIn("ao", committed["bake"])
        self.assertNotIn("indirect", physical)
        self.assertNotIn("ao", physical["bake"])
        del committed["indirect"], committed["bake"]["ao"]
        self.assertEqual(physical, committed)

    def test_a_table_right_after_indirect_is_kept(self):
        text = "[indirect]\nintensity = 2.0\n[bake]\nray_offset = 0.5\nao = { distance = 1.0, rays = 2 }\nindirect = { bounces = 1, rays = 4 }\n"
        self.assertEqual(self.module.physical_scene(text), "[bake]\nray_offset = 0.5\nindirect = { bounces = 1, rays = 4 }\n")

    def test_an_ao_key_outside_bake_is_left_alone(self):
        text = "[bake]\nray_offset = 0.5\n\n[[objects]]\nname = \"a\"\nao = 1\n"
        self.assertEqual(self.module.physical_scene(text), text)

    def test_a_copy_in_another_folder_still_reaches_the_camera_animation(self):
        from r3d.import_settings import load_scene
        scene = DEMO_SCENE
        text = self.module.physical_scene(scene.read_text(encoding="utf-8"), scene.parent)
        with tempfile.TemporaryDirectory() as directory:
            copy = pathlib.Path(directory) / "study" / scene.name
            copy.parent.mkdir()
            copy.write_text(text, encoding="utf-8")
            (copy.parent / DEMO_IMPORT.name).write_text(
                DEMO_IMPORT.read_text(encoding="utf-8"), encoding="utf-8")
            path = load_scene(copy).camera.component.path
            self.assertIsNotNone(path)


if __name__ == "__main__":
    unittest.main()
