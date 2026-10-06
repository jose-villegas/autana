"""find_python in python.sh, run by a real shell over a real venv: the first
interpreter on PATH wins whatever it is named, so a Windows venv, which holds
only `python`, is not passed over for a `python3` later on PATH."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

LIB = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LIB.parents[0] / "device"))
import device  # noqa: E402


class FindPythonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.shell = device.git_bash() if os.name == "nt" else "sh"
        except RuntimeError as error:
            raise unittest.SkipTest(str(error))
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        venv = root / "venv"
        subprocess.run([sys.executable, "-m", "venv", "--without-pip", str(venv)], check=True,
                       capture_output=True, timeout=300)
        cls.venv_bin = venv / ("Scripts" if os.name == "nt" else "bin")
        # Linux venvs also hold python3, which python links to; drop it so
        # both platforms see the Windows layout.
        if os.name != "nt":
            python = cls.venv_bin / "python"
            target = os.path.realpath(python)
            python.unlink()
            python.symlink_to(target)
            (cls.venv_bin / "python3").unlink()
        site = subprocess.run([str(cls.venv_bin / "python"), "-c",
                               "import sysconfig; print(sysconfig.get_path('purelib'))"],
                              check=True, capture_output=True, text=True, timeout=60)
        Path(site.stdout.strip(), "venv_only_marker.py").write_text("")
        cls.other_bin = root / "other"
        cls.other_bin.mkdir()
        python3 = cls.other_bin / "python3"
        python3.write_text(f'#!/bin/sh\nexec "{Path(sys.executable).as_posix()}" "$@"\n')
        python3.chmod(0o755)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def prefix_of(self, path, *modules):
        """sys.prefix of what find_python picks with PATH set to `path`."""
        script = (f'. "{(LIB / "python.sh").as_posix()}" && py=$(find_python {" ".join(modules)}) '
                  '&& "$py" -c "import sys; print(sys.prefix)"')
        env = dict(os.environ, PATH=os.pathsep.join(str(entry) for entry in path))
        env.pop("PYTHONPATH", None)
        result = subprocess.run([self.shell, "-c", script], env=env, capture_output=True,
                                text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return Path(result.stdout.strip()).resolve()

    def test_a_venv_first_on_path_wins_over_a_later_python3(self):
        self.assertEqual(self.prefix_of([self.venv_bin, self.other_bin]),
                         self.venv_bin.parent.resolve())

    def test_an_interpreter_missing_a_module_is_passed_over(self):
        self.assertEqual(self.prefix_of([self.other_bin, self.venv_bin], "venv_only_marker"),
                         self.venv_bin.parent.resolve())

    def test_without_the_venv_the_later_python3_is_found(self):
        self.assertNotEqual(self.prefix_of([self.other_bin]), self.venv_bin.parent.resolve())


if __name__ == "__main__":
    unittest.main()
