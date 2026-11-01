"""packages.sh lists a launcher's packages as packages.py does, and engine_sources.cmake the
renderer's sources; check_packages_alone.sh fails a package that needs more than its own include
root and those of the packages it declares."""
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
sys.path.insert(0, str(ROOT / "scripts/device/tests"))
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)
import device  # noqa: E402
import package_requires  # noqa: E402
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


class PackageRequires(FixtureLauncher):
    """Both twins read `set(<name>_requires ...)` the same way and follow it to every package."""

    def setUp(self):
        super().setUp()
        for name in ("alpha", "beta", "gamma", "delta"):
            self.write(f"packages/{name}/include/{name}/{name}.h")
        self.write("packages/alpha/CMakeLists.txt", "# alpha\nset(alpha_requires beta  gamma)\nadd_library(alpha)\n")
        self.write("packages/beta/CMakeLists.txt", "set(beta_requires delta)\n")
        self.write("packages/gamma/CMakeLists.txt", "set(gamma_requires)\n")
        self.write("packages/delta/CMakeLists.txt", "set(delta_requires alpha)\n")

    def shell(self, function, package):
        package_dir = (self.launcher / "packages" / package).as_posix()
        result = run_shell("-c", f'. "{(BUILD / "packages.sh").as_posix()}"\n{function} "{package_dir}"\n')
        self.assertEqual(0, result.returncode, result.stderr)
        return result.stdout.splitlines()

    def test_requires_reads_the_packages_own_line_in_order(self):
        self.assertEqual(("beta", "gamma"), package_requires.requires(self.launcher / "packages/alpha"))
        self.assertEqual(["beta", "gamma"], self.shell("package_requires", "alpha"))

    def test_an_empty_or_missing_line_requires_nothing(self):
        for name in ("gamma", "missing"):
            with self.subTest(name=name):
                self.assertEqual((), package_requires.requires(self.launcher / "packages" / name))
                self.assertEqual([], self.shell("package_requires", name))

    def test_another_packages_line_is_not_this_ones(self):
        self.write("packages/gamma/CMakeLists.txt", "set(alpha_requires beta)\n")
        self.assertEqual((), package_requires.requires(self.launcher / "packages/gamma"))
        self.assertEqual([], self.shell("package_requires", "gamma"))

    def test_reachable_follows_declarations_through_a_cycle_once_each(self):
        twin = [path.name for path in package_requires.reachable(self.launcher / "packages/alpha")]
        self.assertEqual(["alpha", "beta", "gamma", "delta"], twin)
        self.assertEqual(twin, [Path(line).name for line in self.shell("package_reachable", "alpha")])

    def test_reachable_skips_a_name_with_no_package(self):
        self.write("packages/gamma/CMakeLists.txt", "set(gamma_requires nowhere)\n")
        self.assertEqual(["gamma"], [path.name for path in package_requires.reachable(self.launcher / "packages/gamma")])
        self.assertEqual(["gamma"], [Path(line).name for line in self.shell("package_reachable", "gamma")])


@unittest.skipUnless(shutil.which("cmake"), "no CMake")
class EngineSources(FixtureLauncher):
    def sources(self):
        script = self.launcher / "tools/render/engine_sources.cmake"
        script.parent.mkdir(parents=True)
        shutil.copy(ROOT / "launcher/tools/render/engine_sources.cmake", script)
        result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, timeout=120)
        self.assertEqual(0, result.returncode, result.stderr)
        return [Path(line).relative_to(self.launcher).as_posix() for line in result.stderr.splitlines()]

    def test_every_c_file_below_a_named_folder_and_nothing_else(self):
        for name in ("main/alpha/x.c", "main/alpha/deep/y.c", "main/alpha/x.h", "main/beta/z.c", "main/gamma/w.c",
                     "main/gfx/a.c", "main/gfx/a_device.c"):
            self.write(name)
        self.write("main/renderer_folders.txt", "# not a folder\nalpha\nbeta\n")
        listed = self.sources()
        self.assertEqual(["main/alpha/deep/y.c", "main/alpha/x.c", "main/beta/z.c"], listed[-3:])
        self.assertIn("main/gfx/a.c", listed)
        self.assertNotIn("main/gfx/a_device.c", listed)
        self.assertNotIn("main/gamma/w.c", listed)


@unittest.skipUnless(shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or os.environ.get("CC"),
                     "no C compiler")
class PackagesAlone(FixtureLauncher):
    def check(self):
        return run_shell(ALONE.as_posix(), self.launcher.as_posix())

    def self_contained(self):
        self.write("packages/p/include/p/p.h", "#pragma once\nint p_value(void);\n")
        self.write("packages/p/src/p.c", '#include "p/p.h"\nint p_value(void) { return 1; }\n')

    def fails_naming(self, *names):
        """Runs the check, which must fail naming each of `names`; returns its output."""
        result = self.check()
        self.assertNotEqual(0, result.returncode)
        for name in names:
            self.assertIn(name, result.stdout)
        return result.stdout

    def test_a_self_contained_package_passes(self):
        self.self_contained()
        result = self.check()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_a_file_that_reaches_outside_the_package_fails_naming_only_it(self):
        rows = (("main/microui.h", "packages/p/include/p/ui.h", '#pragma once\n#include "microui.h"\n',
                 "ui.h", "FAIL p: p/p.h"),
                ("main/esp_timer.h", "packages/p/src/clock.c",
                 '#include "esp_timer.h"\nint p_clock(void) { return 0; }\n', "src/clock.c", "src/p.c"))
        for outside, reaching, text, named, unnamed in rows:
            with self.subTest(reaching=reaching):
                self.setUp()
                self.self_contained()
                self.write(outside, "#pragma once\n")
                self.write(reaching, text)
                self.assertNotIn(unnamed, self.fails_naming(named))

    def test_a_declared_package_is_on_the_include_path_and_an_undeclared_one_is_not(self):
        self.self_contained()
        self.write("packages/q/include/q/q.h", "#pragma once\nint q_value(void);\n")
        self.write("packages/r/include/r/r.h", '#pragma once\n#include "q/q.h"\n')
        self.write("packages/p/include/p/uses.h", '#pragma once\n#include "r/r.h"\n')
        self.fails_naming("FAIL p: p/uses.h", "FAIL r: r/r.h")
        self.write("packages/p/CMakeLists.txt", "set(p_requires r)\n")
        self.write("packages/r/CMakeLists.txt", "set(r_requires q)\n")
        result = self.check()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_tests_and_benches_cannot_reach_engine_headers(self):
        self.self_contained()
        self.write("test/suites.h", "#pragma once\n")
        self.write("packages/p/tests/suite_p.c", '#include "suites.h"\n')
        self.write("packages/p/bench/measure.c", '#include "suites.h"\n')
        self.fails_naming("tests/suite_p.c", "bench/measure.c")

    def test_tests_get_local_headers_testkit_and_unity(self):
        self.self_contained()
        self.write("packages/testkit/include/testkit/suite.h", "#pragma once\n")
        self.write("packages/testkit/unity/unity.h", "#pragma once\n")
        self.write("packages/p/tests/local.h", "#pragma once\n")
        self.write("packages/p/tests/suite_p.c",
                   '#include "local.h"\n#include "testkit/suite.h"\n#include "unity.h"\n')
        result = self.check()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_no_packages_is_a_failure_not_a_pass(self):
        self.assertNotEqual(0, self.check().returncode)


@unittest.skipUnless(shutil.which("gcc") or shutil.which("cc") or shutil.which("clang"), "no C compiler")
class StandaloneRunner(unittest.TestCase):
    WRAP_COUNT = 1 << 8
    DROP_ENV = "TESTKIT_SMOKE_DROPS"

    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temp.cleanup)
        work = Path(temp.name)
        fixture = work / "suite.c"
        fixture.write_text(r'''#include <stdlib.h>
#include "testkit/suite.h"
#include "unity.h"

#define EXIT_STATUS_WRAP (1 << 8)
#define DROP_ENV "TESTKIT_SMOKE_DROPS"

static void fail(void) {
    TEST_FAIL_MESSAGE("runner exit status regression");
}

static void failures(void) {
    for (int i = 0; i < EXIT_STATUS_WRAP; i++) {
        RUN_TEST(fail);
    }
}
SUITE_REGISTER_ON_REQUEST(failures)

__attribute__((constructor)) static void drops(void) {
    if (getenv(DROP_ENV) != NULL) {
        for (int i = 0; i < EXIT_STATUS_WRAP; i++) {
            suites_record_drop();
        }
    }
}
''', encoding="utf-8")
        testkit = ROOT / "launcher/packages/testkit"
        (work / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.21)
project(standalone_runner C)
add_subdirectory("{testkit.as_posix()}" testkit)
add_executable(runner suite.c)
target_link_libraries(runner PRIVATE testkit_runner)
''', encoding="utf-8")
        build = work / "build"
        cls.runner = build / ("runner.exe" if os.name == "nt" else "runner")
        for command in (["cmake", "-S", str(work), "-B", str(build), "-G", "Unix Makefiles"],
                        ["cmake", "--build", str(build), "--target", "runner"]):
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)

    def run_runner(self, *args, drops=False):
        env = os.environ.copy()
        env.pop(self.DROP_ENV, None)
        if drops:
            env[self.DROP_ENV] = "1"
        return subprocess.run([str(self.runner), *args], env=env,
                              capture_output=True, text=True, timeout=30)

    def test_success_and_listing_return_success(self):
        for args in ((), ("--list",)):
            with self.subTest(args=args):
                result = self.run_runner(*args)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_on_request_runs_all_on_request_suites(self):
        result = self.run_runner("--on-request")
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(f"{self.WRAP_COUNT} Tests {self.WRAP_COUNT} Failures", result.stdout)
        self.assertNotIn("Unknown suite", result.stderr)

    def test_full_exit_status_range_of_failures_returns_failure(self):
        result = self.run_runner("failures")
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(f"{self.WRAP_COUNT} Tests {self.WRAP_COUNT} Failures", result.stdout)

    def test_full_exit_status_range_of_unknown_suites_returns_failure(self):
        result = self.run_runner(*(["unknown"] * self.WRAP_COUNT))
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertEqual(self.WRAP_COUNT, result.stderr.count("Unknown suite: unknown"))

    def test_full_exit_status_range_of_drops_fails_run_and_listing(self):
        for args in ((), ("--list",)):
            with self.subTest(args=args):
                result = self.run_runner(*args, drops=True)
                self.assertEqual(1, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
