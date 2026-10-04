"""Failures in image generation must identify the command and its error."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "launcher/tools/render/render_doc_images.sh"
APP_SCRIPT = ROOT / "launcher/main/apps/render_lab/tools/doc_images.sh"


@unittest.skipUnless(shutil.which("bash"), "needs bash")
class DocImageFailureTest(unittest.TestCase):
    def run_failure(self, command):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            script = root / "launcher/tools/render/render_doc_images.sh"
            script.parent.mkdir(parents=True)
            setup = SCRIPT.read_text(encoding="utf-8").split("# The one orphan report:")[0]
            script.write_text(setup + command, encoding="utf-8")
            return subprocess.run(["bash", script.as_posix()], capture_output=True, text=True)

    def test_failed_command_is_named(self):
        result = self.run_failure("missing_doc_image_command\n")
        self.assertEqual(result.returncode, 2)
        self.assertIn("command not found", result.stderr)
        self.assertRegex(result.stderr, r"failed.*missing_doc_image_command")

    def test_nested_bake_error_is_visible(self):
        result = self.run_failure(
            'mkdir -p "$WORK/app/variant"\n'
            'sh -c "echo missing_meshoptimizer >&2; exit 7" > "$WORK/app/variant/bake.log" 2>&1\n'
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
            function = source.split("variant_bake() {", 1)[1].split("\nvariant_bake direct", 1)[0]
            script = root / "stage.sh"
            script.write_text(
                'set -e\nW=work\nM=.\nR3D_PYTHON=sh\n'
                'variant_bake() {' + function + "\nvariant_bake direct none ''\n", encoding="utf-8")
            result = subprocess.run(["bash", script.as_posix()], cwd=root, capture_output=True, text=True)
            log = (root / "work/indirect-direct/bake.log").read_text(encoding="utf-8")
            self.assertEqual(result.returncode, 8, result.stderr + log)
            self.assertIn("import_dependency_ready", log)


if __name__ == "__main__":
    unittest.main()
