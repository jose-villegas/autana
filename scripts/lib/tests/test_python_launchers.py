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
        (self.root / "scripts/lib/python-min-version").write_text(MINIMUM + "\n")
        (self.root / "scripts/autana/autana.py").write_text(
            'import sys\nprint("STARTED", sys.version_info[:2], sys.argv[1:])\n')
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.bin / "python.bat").write_text('@echo off\necho 3.9\nexit /b 0\n')
        self.py = self.bin / "py.bat"
        self.py.write_text('@echo off\necho 3.9\nexit /b 0\n')

    def launch(self):
        env = dict(os.environ, PATH=str(self.bin), IDF_TOOLS_PATH=str(self.root / "idf"))
        command = (f'"{os.environ["COMSPEC"]}" /d /s /c '
                   f'""{self.root / "tools/autana.cmd"}" "argument with spaces""')
        return subprocess.run(command, cwd=self.root, env=env, capture_output=True,
                              text=True, timeout=60)

    def test_skips_old_python_for_versioned_py(self):
        self.py.write_text('@echo off\nif not "%1" == "-3" exit /b 2\n'
                           f'"{sys.executable}" %2 %3 %4 %5 %6 %7 %8 %9\nexit /b %errorlevel%\n')
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("STARTED " + str(sys.version_info[:2]), result.stdout)
        self.assertIn("['argument with spaces']", result.stdout)

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
