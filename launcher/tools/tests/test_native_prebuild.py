"""native/prebuild.py finds every library builder in the source and builds them first, so test processes started
after it, many at once, load each library and none builds it."""

import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from native import prebuild  # noqa: E402
from native.shared_library import find_compiler  # noqa: E402

TOOLS = pathlib.Path(__file__).resolve().parents[1]
# Processes that load the library at once, as parallel test files do.
LOADERS = 4

BUILDER = textwrap.dedent('''\
    import pathlib
    import sys
    sys.path.insert(0, {tools!r})
    from native.shared_library import build_shared

    HERE = pathlib.Path(__file__).resolve().parent

    def _library():
        return build_shared("probe", HERE / ".cache", (HERE / "probe.c",), {compiler!r})

    def with_argument(name):
        return build_shared(name, HERE, (), "cc")

    class Holder:
        def method():
            return build_shared("held", HERE, (), "cc")

    def unrelated():
        return 1
''')


class PrebuildTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)  # Windows keeps a DLL this process loaded
        self.addCleanup(temp.cleanup)
        self.root = pathlib.Path(temp.name)
        # Each test's probe package is its own: forget the last one's import and path.
        for name in ("probe_pkg", "probe_pkg.probe_lib"):
            sys.modules.pop(name, None)
        self.addCleanup(lambda: str(self.root) in sys.path and sys.path.remove(str(self.root)))

    def write_builder(self, compiler="cc"):
        package = self.root / "probe_pkg"
        package.mkdir()
        (package / "__init__.py").write_text("")
        (package / "probe.c").write_text("int probe_answer(void) { return 42; }\n")
        module = package / "probe_lib.py"
        module.write_text(BUILDER.format(tools=str(TOOLS), compiler=compiler))
        return module

    def test_only_a_module_level_function_without_arguments_that_calls_build_shared_is_a_builder(self):
        self.assertEqual([("probe_pkg.probe_lib", "_library")], prebuild.builders(self.root, [self.write_builder()]))

    def test_the_tools_own_libraries_are_found_in_their_source(self):
        found = prebuild.builders(TOOLS, prebuild.tracked_sources(TOOLS))
        self.assertIn(("r3d.lod.levels", "_library"), found)
        self.assertIn(("r3d.meshopt", "_library"), found)
        self.assertFalse([module for module, _ in found if module.startswith("tests.")])

    def test_after_the_prebuild_processes_loading_at_once_build_nothing(self):
        try:
            compiler = find_compiler("CC", ("cc", "gcc", "clang"), "C")
        except SystemExit:
            self.skipTest("no C compiler")
        module = self.write_builder(compiler)
        said = []
        self.assertEqual(0, prebuild.build_all(self.root, [module], log=said.append))
        self.assertEqual(["built probe_pkg.probe_lib._library"], said)
        load = ("import sys; sys.path.insert(0, sys.argv[1]); from probe_pkg import probe_lib; "
                "print(probe_lib._library().probe_answer())")
        loaders = [subprocess.Popen([sys.executable, "-c", load, str(self.root)], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, text=True)
                   for _ in range(LOADERS)]
        for loader in loaders:
            output, _ = loader.communicate(timeout=120)
            self.assertEqual(0, loader.returncode, output)
            self.assertEqual("42", output.strip(), "a loader built the library itself")

    def test_a_builder_that_fails_is_counted_and_the_rest_still_build(self):
        module = self.write_builder(compiler=str(self.root / "no-such-compiler"))
        said = []
        self.assertEqual(1, prebuild.build_all(self.root, [module], log=said.append))
        self.assertTrue(said[0].startswith("could not build probe_pkg.probe_lib._library"), said)


if __name__ == "__main__":
    unittest.main()
