"""packages.sh lists a launcher's packages as packages.py does; check_packages_alone.sh
fails a package that needs more than its own include root."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "launcher/tools/build"
sys.path.insert(0, str(ROOT / "scripts/device"))
sys.path.insert(0, str(BUILD))
import device  # noqa: E402
import packages  # noqa: E402

SHELL = device.git_bash() if os.name == "nt" else shutil.which("sh")
ALONE = ROOT / "launcher/test/check_packages_alone.sh"


def run_shell(*args):
    return subprocess.run([SHELL, *args], capture_output=True, text=True, timeout=120)


class FixtureLauncher(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.launcher = Path(temp.name) / "launcher"
        (self.launcher / "packages").mkdir(parents=True)

    def write(self, path, text=""):
        target = self.launcher / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def lines(self, function):
        script = f'. "{(BUILD / "packages.sh").as_posix()}"\n{function} "{self.launcher.as_posix()}"\n'
        result = run_shell("-c", script)
        self.assertEqual(0, result.returncode, result.stderr)
        return result.stdout.splitlines()

    def names(self, function):
        return [Path(line).relative_to(self.launcher.as_posix()).as_posix() for line in self.lines(function)]


class PackageListing(FixtureLauncher):
    def setUp(self):
        super().setUp()
        for name in ("alpha", "beta"):
            self.write(f"packages/{name}/include/{name}/{name}.h")
            self.write(f"packages/{name}/src/deep/{name}.c")
            self.write(f"packages/{name}/src/{name}.h")
            self.write(f"packages/{name}/tests/suite_{name}.c")
            self.write(f"packages/{name}/tests/helper.c")
        self.write("packages/loose/README.md")

    def test_include_roots_are_each_packages_include_folder(self):
        self.assertEqual(["packages/alpha/include", "packages/beta/include"], self.names("package_include_dirs"))

    def test_include_roots_agree_with_the_python_twin(self):
        twin = [path.relative_to(self.launcher).as_posix() for path in packages.include_dirs(self.launcher)]
        self.assertEqual(twin, self.names("package_include_dirs"))

    def test_sources_are_the_c_files_under_src(self):
        self.assertEqual(["packages/alpha/src/deep/alpha.c", "packages/beta/src/deep/beta.c"],
                         self.names("package_sources"))

    def test_suites_are_only_the_suite_files_under_tests(self):
        self.assertEqual(["packages/alpha/tests/suite_alpha.c", "packages/beta/tests/suite_beta.c"],
                         self.names("package_suites"))

    def test_no_packages_prints_nothing_and_succeeds(self):
        shutil.rmtree(self.launcher / "packages")
        (self.launcher / "packages").mkdir()
        for function in ("package_include_dirs", "package_includes", "package_sources", "package_suites"):
            with self.subTest(function=function):
                self.assertEqual([], self.lines(function))


@unittest.skipUnless(shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or os.environ.get("CC"),
                     "no C compiler")
class PackagesAlone(FixtureLauncher):
    def check(self):
        return run_shell(ALONE.as_posix(), self.launcher.as_posix())

    def self_contained(self):
        self.write("packages/p/include/p/p.h", "#pragma once\nint p_value(void);\n")
        self.write("packages/p/src/p.c", '#include "p/p.h"\nint p_value(void) { return 1; }\n')

    def test_a_self_contained_package_passes(self):
        self.self_contained()
        result = self.check()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_a_header_that_reaches_outside_the_package_fails_naming_it(self):
        self.self_contained()
        self.write("main/microui.h", "#pragma once\n")
        self.write("packages/p/include/p/ui.h", '#pragma once\n#include "microui.h"\n')
        result = self.check()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("ui.h", result.stdout)
        self.assertNotIn("FAIL p: p/p.h", result.stdout)

    def test_a_source_that_reaches_outside_the_package_fails_naming_it(self):
        self.self_contained()
        self.write("main/esp_timer.h", "#pragma once\n")
        self.write("packages/p/src/clock.c", '#include "esp_timer.h"\nint p_clock(void) { return 0; }\n')
        result = self.check()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("src/clock.c", result.stdout)
        self.assertNotIn("src/p.c", result.stdout)

    def test_no_packages_is_a_failure_not_a_pass(self):
        self.assertNotEqual(0, self.check().returncode)


if __name__ == "__main__":
    unittest.main()
