"""Failures in image generation name the command and its error; figures ask only for bakes the tree makes."""

import os
import pathlib
import shutil
import subprocess
import sys
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
            packages = root / "launcher/tools/build/packages.sh"
            packages.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / "launcher/tools/build/packages.sh", packages)
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

    def test_only_the_failed_steps_log_is_printed(self):
        result = self.run_failure(
            'mkdir -p "$WORK/app"\n'
            'run sh -c "echo healthy_render_line >&2" 2> "$WORK/app/still.log"\n'
            'run sh -c "echo broken_bake_line >&2; exit 7" 2> "$WORK/app/bake.log"\n'
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("broken_bake_line", result.stderr)
        self.assertNotIn("healthy_render_line", result.stderr)
        self.assertNotIn("app/still.log", result.stderr)


@unittest.skipUnless(shutil.which("sh"), "needs sh")
class FigureBakesTest(unittest.TestCase):
    def test_figures_ask_for_outputs_the_tree_bakes(self):
        """The script's own bake lookup, run on the tree with `bake.py path` logged instead of fetched: each name
        it asks for is an output `bake.py list` reports."""
        lines = SCRIPT.read_text(encoding="utf-8").splitlines()
        start = next(index for index, line in enumerate(lines) if line.startswith("# The cached bakes the figures draw"))
        end = next(index for index in range(start, len(lines)) if lines[index].startswith("tree_mesh="))
        settings = [line for line in lines if line.startswith(("DEMO_SCENE=", "DEMO_OBJECT=", "SKIN_BAKE="))]
        with tempfile.TemporaryDirectory() as directory:
            asked = pathlib.Path(directory) / "asked"
            shim = pathlib.Path(directory) / "python"
            shim.write_text('#!/bin/sh\nif [ "$2" = path ]; then echo "$3" >> "$ASKED"; echo "cache/$3"; exit 0; fi\n'
                            f'exec "{pathlib.Path(sys.executable).as_posix()}" "$@"\n')
            shim.chmod(0o755)
            block = "\n".join([f"PYTHON='{shim.as_posix()}'"] + settings + lines[start:end + 1]) + "\n"
            done = subprocess.run(["sh", "-c", block], cwd=ROOT, capture_output=True, text=True,
                                  env={**os.environ, "ASKED": asked.as_posix()})
            self.assertEqual(done.returncode, 0, done.stderr)
            names = asked.read_text().split()
        listed = subprocess.run([sys.executable, "launcher/tools/bake/bake.py", "list"], cwd=ROOT, capture_output=True,
                                text=True, check=True).stdout
        outputs = {line.split("\t")[0] for line in listed.splitlines()}
        self.assertTrue(names, "the block asked for no bake")
        self.assertEqual([name for name in names if name not in outputs], [])


if __name__ == "__main__":
    unittest.main()
