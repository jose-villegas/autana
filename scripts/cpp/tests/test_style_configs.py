"""The C++ configs carry a banner naming the command that makes them, as typed in the project."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "style_configs.py"
ROOT = SCRIPT.parents[2]
GENERATED = ROOT / "scripts/gates/check_generated_files.py"


class StyleConfigTests(unittest.TestCase):
    def make(self, tool, cwd):
        return subprocess.run([sys.executable, str(SCRIPT), tool], cwd=cwd, check=True,
                              capture_output=True).stdout.decode("utf-8")

    def test_configs_regenerate_and_a_drift_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            project = pathlib.Path(temp).resolve()
            subprocess.check_call(["git", "init", "-q"], cwd=project)
            # The project holds this checkout where its banner command says.
            engine = project / "engine"
            engine.symlink_to(ROOT, target_is_directory=True) if sys.platform != "win32" else None
            if not engine.exists():
                subprocess.check_call(["cmd", "/c", "mklink", "/J", str(engine), str(ROOT)],
                                      stdout=subprocess.DEVNULL)
            script = engine / "scripts/cpp/style_configs.py"
            for tool in ("clang-format", "clang-tidy"):
                text = subprocess.run([sys.executable, str(script), tool], cwd=project, check=True,
                                      capture_output=True).stdout.decode("utf-8")
                self.assertIn(f"# python engine/scripts/cpp/style_configs.py {tool} > .{tool}\n", text)
                (project / f".{tool}").write_text(text, encoding="utf-8", newline="\n")
            subprocess.check_call(["git", "add", ".clang-format", ".clang-tidy"], cwd=project)
            check = [sys.executable, str(GENERATED), ".clang-format", ".clang-tidy"]
            ok = subprocess.run(check, cwd=project, capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
            with open(project / ".clang-tidy", "a", encoding="utf-8", newline="\n") as tidy:
                tidy.write("  extra-check: true\n")
            drifted = subprocess.run(check, cwd=project, capture_output=True, text=True)
            self.assertNotEqual(drifted.returncode, 0)
            self.assertIn(".clang-tidy", drifted.stdout + drifted.stderr)
            if sys.platform == "win32":
                subprocess.check_call(["cmd", "/c", "rmdir", str(engine)])

    def test_clang_format_drops_the_engine_header_comment(self):
        text = self.make("clang-format", ROOT)
        body = text.split("\n\n", 1)[1]
        self.assertFalse(body.startswith("#"), body[:80])


if __name__ == "__main__":
    unittest.main()
