"""Failures in image generation must identify the command and its error."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "launcher/tools/render/render_doc_images.sh"


@unittest.skipUnless(shutil.which("sh"), "needs sh")
class EngineDocImageFailureTest(unittest.TestCase):
    def run_failure(self, command):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            script = root / "launcher/tools/render/render_doc_images.sh"
            script.parent.mkdir(parents=True)
            helper = root / "scripts/lib/run.sh"
            helper.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / "scripts/lib/run.sh", helper)
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


if __name__ == "__main__":
    unittest.main()
