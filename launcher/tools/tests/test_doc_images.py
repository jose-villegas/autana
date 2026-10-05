"""Failures in image generation must identify the command and its error."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "launcher/tools/render/render_doc_images.sh"
APP_SCRIPT = ROOT / "launcher/main/apps/render_lab/tools/doc_images.sh"


@unittest.skipUnless(shutil.which("sh"), "needs sh")
class DocImageFailureTest(unittest.TestCase):
    def run_failure(self, command):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            script = root / "launcher/tools/render/render_doc_images.sh"
            script.parent.mkdir(parents=True)
            setup = SCRIPT.read_text(encoding="utf-8").split("# The one orphan report:")[0]
            script.write_text(setup + command, encoding="utf-8")
            return subprocess.run(["sh", script.as_posix()], capture_output=True, text=True)

    def test_failed_command_is_named(self):
        result = self.run_failure("run missing_doc_image_command\n")
        self.assertEqual(result.returncode, 2)
        self.assertIn("not found", result.stderr)
        self.assertRegex(result.stderr, r"failed.*missing_doc_image_command")

    def test_nested_bake_error_is_visible(self):
        result = self.run_failure(
            'mkdir -p "$WORK/app/variant"\n'
            'run sh -c "echo missing_meshoptimizer >&2; exit 7" > "$WORK/app/variant/bake.log" 2>&1\n'
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("missing_meshoptimizer", result.stderr)
        self.assertIn("app/variant/bake.log", result.stderr)

    def test_variant_scene_keeps_its_import_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            meshes = root / "meshes"
            shutil.copytree(ROOT / "launcher/main/apps/render_lab/meshes", meshes,
                            ignore=shutil.ignore_patterns("*.mesh", "*.c", "*.h"))
            importer = root / "launcher/tools/r3d/mesh_import.py"
            importer.parent.mkdir(parents=True)
            importer.write_text(
                'test -f "$(dirname "$1")/sponza.import.toml" || exit 7\n'
                'echo import_dependency_ready\nexit 8\n', encoding="utf-8")
            source = APP_SCRIPT.read_text(encoding="utf-8")
            helper = "run() {" + source.split("run() {", 1)[1].split("\n}\n", 1)[0] + "\n}\n"
            functions = source.split("bake_and_render() {", 1)[1].split("\nvariant_bake smooth", 1)[0]
            functions = "\n".join(line for line in functions.splitlines() if "physical_scene.py" not in line)
            script = root / "stage.sh"
            script.write_text(
                'set -e\nW=work\nM=.\nR3D_PYTHON=sh\nmkdir -p work\ncp meshes/sponza.scene.toml work/physical.scene.toml\n'
                + helper + 'bake_and_render() {' + functions + "\nvariant_bake direct none ''\n", encoding="utf-8")
            result = subprocess.run(["sh", script.as_posix()], cwd=root, capture_output=True, text=True)
            log = (root / "work/indirect-direct/bake.log").read_text(encoding="utf-8")
            self.assertEqual(result.returncode, 8, result.stderr + log)
            self.assertIn("import_dependency_ready", log)


class PhysicalSceneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import importlib.util
        path = ROOT / "launcher/main/apps/render_lab/tools/physical_scene.py"
        spec = importlib.util.spec_from_file_location("physical_scene", path)
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def test_the_committed_scene_loses_its_indirect_table_and_occlusion_and_nothing_else(self):
        import tomllib
        text = (ROOT / "launcher/main/apps/render_lab/meshes/sponza.scene.toml").read_text(encoding="utf-8")
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


if __name__ == "__main__":
    unittest.main()
