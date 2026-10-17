"""Python discovery through real shell processes and interpreter imports."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

LIB = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LIB.parents[0] / "device"))
sys.path.insert(0, str(LIB.parent / "device" / "tests"))
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)
import device  # noqa: E402


class FindPythonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.shell = device.git_bash() if os.name == "nt" else shutil.which("sh")
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
        result = subprocess.run([self.shell, "-c", script, str(LIB / "python.sh")],
                                env=env, capture_output=True,
                                text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return Path(result.stdout.strip()).resolve()

    def test_pinned_minor_preference_and_module_fallback(self):
        pin = (LIB.parents[1] / ".python-version").read_text().strip()
        minor = pin.rsplit(".", 1)[0]
        minimum = (LIB / "python-min-version").read_text().strip()
        with tempfile.TemporaryDirectory(prefix="python pin ") as temporary:
            root = Path(temporary)
            older_bin, pinned_bin = root / "older", root / "pinned"
            older_bin.mkdir()
            pinned_bin.mkdir()
            older = older_bin / "python3"
            pinned = pinned_bin / f"python{minor}"
            probes = root / "probes"
            for candidate, interpreter, version in (
                    (older, self.venv_bin / "python", minimum),
                    (pinned, Path(sys.executable), minor)):
                candidate.write_text(
                    f'#!/bin/sh\nexec "{interpreter.as_posix()}" -c \'\n'
                    'import sys\n'
                    f'sys.version_info = tuple(map(int, "{version}".split(".")))\n'
                    f'with open({json.dumps(probes.as_posix())}, "a") as log: log.write({json.dumps(candidate.name)} + "\\n")\n'
                    'code = sys.argv[2]; sys.argv = sys.argv[2:]; exec(code)\n'
                    "' \"$@\"\n")
                candidate.chmod(0o755)
            script = f'. "{(LIB / "python.sh").as_posix()}"; find_python "$@"'
            for case, modules, available, expected, count in (
                    ("preferred", [], True, pinned, 1),
                    ("module fallback", ["venv_only_marker"], True, older, 2),
                    ("absent", [], False, older, 1)):
                with self.subTest(case=case):
                    if not available:
                        pinned.unlink()
                    probes.write_text("")
                    result = subprocess.run(
                        [self.shell, "-c", script, str(LIB / "python.sh"), *modules],
                        env=dict(os.environ, PATH=os.pathsep.join(
                            str(entry) for entry in (older_bin, pinned_bin))),
                        capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(Path(result.stdout.strip()).parts[-2:], expected.parts[-2:])
                    self.assertEqual(len(probes.read_text().splitlines()), count)

    def test_an_older_python3_is_passed_over(self):
        older = Path(self.temp.name) / "older"
        older.mkdir(exist_ok=True)
        python3 = older / "python3"
        python3.write_text(f'''#!/bin/sh
exec "{Path(sys.executable).as_posix()}" -c 'import sys; sys.version_info = (3, 9, 0); code = sys.argv[2]; sys.argv = sys.argv[2:]; exec(code)' "$@"
''')
        python3.chmod(0o755)
        self.assertEqual(self.prefix_of([older, self.venv_bin]),
                         self.venv_bin.parent.resolve())

    def test_no_matching_python_names_the_minimum(self):
        script = f'. "{(LIB / "python.sh").as_posix()}"; find_python'
        result = subprocess.run([self.shell, "-c", script, str(LIB / "python.sh")],
                                env=dict(os.environ, PATH=str(Path(self.temp.name) / "empty")),
                                capture_output=True, text=True, timeout=60)
        minimum = (LIB / "python-min-version").read_text().strip()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Python >= " + minimum, result.stderr)

    @unittest.skipIf(os.name == "nt", "Linux versioned executable names")
    def test_versioned_python_names_are_tried_newest_first(self):
        versioned = Path(self.temp.name) / "versioned"
        versioned.mkdir()
        for name in ("python3.9", "python3.12", "python3.13"):
            shutil.copy(self.other_bin / "python3", versioned / name)
        shutil.copy(self.other_bin / "python3", versioned / "python3.99-config")
        script = f'. "{(LIB / "python.sh").as_posix()}"; find_python'
        result = subprocess.run([self.shell, "-c", script, str(LIB / "python.sh")],
                                env=dict(os.environ, PATH=str(versioned)),
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(versioned / "python3.13"))

    def test_a_source_outside_the_tree_cannot_use_the_working_directory(self):
        outside = Path(self.temp.name) / "python.sh"
        shutil.copy(LIB / "python.sh", outside)
        script = f'. "{outside.as_posix()}"'
        result = subprocess.run([self.shell, "-c", script, str(outside)],
                                cwd=LIB.parents[1], capture_output=True, text=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot locate scripts/lib/python-min-version", result.stderr)

    def test_bootstrap_skips_an_older_idf_python(self):
        root = Path(self.temp.name) / "bootstrap"
        for relative in ("tools/autana", "scripts/lib/python.sh",
                         "scripts/lib/python-min-version", "scripts/lib/process_pid.sh"):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(LIB.parents[1] / relative, target)
        app = root / "scripts/autana/autana.py"
        app.parent.mkdir()
        app.write_text('import sys; print(sys.version_info[:2])\n')
        binary = root / "bin"
        binary.mkdir()
        dirname = binary / "dirname"
        dirname.write_text(f'#!/bin/sh\nprintf "%s\\n" "{root.as_posix()}/tools"\n')
        dirname.chmod(0o755)
        for name, code in (("idf_a_old_env", "import sys; sys.version_info = (3, 9, 0); "),
                           ("idf_b_matching_env", "")):
            python = root / "idf/python_env" / name / "Scripts/python.exe"
            python.parent.mkdir(parents=True)
            python.write_text(
                f'#!/bin/sh\nexec "{Path(sys.executable).as_posix()}" -c \'\n'
                f"{code}\n"
                'import runpy, sys\n'
                'args = sys.argv[1:]\n'
                'command = args[0] == "-c"\n'
                'target = args[1] if command else args[0]\n'
                'sys.argv = args[1:] if command else args\n'
                'exec(target) if command else runpy.run_path(target, run_name="__main__")\n'
                "' \"$@\"\n")
            python.chmod(0o755)
        result = subprocess.run([self.shell, str(root / "tools/autana")],
                                env=dict(os.environ, PATH=str(binary),
                                         IDF_TOOLS_PATH=str(root / "idf")),
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(sys.version_info[:2]), result.stdout)

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
