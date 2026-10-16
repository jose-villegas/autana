"""Launchers cross real shell and interpreter process boundaries."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
MINIMUM = (ROOT / "scripts/lib/python-min-version").read_text().strip()


@unittest.skipUnless(os.name == "nt", "cmd.exe requires Windows")
class CmdLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="python launch ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "tools").mkdir()
        (self.root / "scripts/autana").mkdir(parents=True)
        shutil.copy(ROOT / "tools/autana.cmd", self.root / "tools/autana.cmd")
        (self.root / "scripts/lib").mkdir()
        shutil.copy(ROOT / "scripts/lib/python-min-version",
                    self.root / "scripts/lib/python-min-version")
        (self.root / "scripts/autana/autana.py").write_text(
            'import sys\nprint("STARTED", sys.version_info[:2], sys.argv[1:])\n')
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.stub = self.bin / "interpreter.py"
        self.stub.write_text(
            'import runpy, sys\n'
            'version = sys.argv.pop(1)\n'
            'sys.argv.pop(0)\n'
            'if sys.argv[0] == "-3": sys.argv.pop(0)\n'
            'if version == "old": sys.version_info = (3, 9, 0)\n'
            'if sys.argv[0] == "-c":\n'
            '    code = sys.argv.pop(1); exec(code)\n'
            'else: runpy.run_path(sys.argv[0], run_name="__main__")\n')
        self.launcher = self.root / "tools/autana.cmd"
        self.original = self.launcher.read_bytes()

    def launch(self, py_version="old", arguments=None):
        launcher = self.original.decode()
        for label in ("probe", "report"):
            launcher = launcher.replace(f'call :{label} python',
                        f'call :{label} "{sys.executable}" "{self.stub}" old')
            launcher = launcher.replace(f'call :{label} py -3',
                        f'call :{label} "{sys.executable}" "{self.stub}" {py_version} -3')
        self.launcher.write_bytes(launcher.encode())
        arguments = arguments or ["argument with spaces"]
        env = dict(os.environ, PATH=str(self.bin), IDF_TOOLS_PATH=str(self.root / "idf"),
                   launch_arguments=" ".join(f'"{arg}"' for arg in arguments), NAME="expanded")
        command = (f'"{os.environ["COMSPEC"]}" /d /s /c '
                   f'""{self.launcher}" %launch_arguments%"')
        return subprocess.run(command, cwd=self.root, env=env, capture_output=True,
                              text=True, timeout=60)

    def test_skips_old_python_for_versioned_py(self):
        result = self.launch(py_version="current")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("STARTED " + str(sys.version_info[:2]), result.stdout)
        self.assertIn("['argument with spaces']", result.stdout)

    def test_argument_fidelity(self):
        arguments = ["argument with spaces", "50%", "q^r", "%NAME%"]
        result = self.launch(py_version="current", arguments=arguments)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(repr(arguments), result.stdout)

    def test_skips_old_python_and_py_for_idf_venv(self):
        venv = self.root / "idf/python_env/idf_matching_env"
        subprocess.run([sys.executable, "-m", "venv", "--without-pip", str(venv)],
                       check=True, capture_output=True, timeout=300)
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("STARTED " + str(sys.version_info[:2]), result.stdout)

    def test_none_matches_names_required_and_found_versions(self):
        result = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(MINIMUM, result.stderr)
        self.assertIn("3.9", result.stderr)
        self.assertNotIn("STARTED", result.stdout)


if __name__ == "__main__":
    unittest.main()
