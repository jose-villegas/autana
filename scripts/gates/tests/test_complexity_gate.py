"""Regression checks for complexity coverage and device command construction."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

QUALITY = Path(__file__).resolve().parents[3] / "launcher/tools/quality"
sys.path.insert(0, str(QUALITY))

import complexity_gate  # noqa: E402
import pinned_tool
import compile_db

DATA_ONLY = """\
/* GENERATED FILE - do not edit. */
#include <stdint.h>
const uint16_t mesh_indices[] = { 0, 1, 2, 2, 3, 0 };
const struct { int a; int b; } mesh_bounds = { 1, 2 };
"""

WITH_FUNCTION = """\
int pick(int a)
{
    if (a > 2) {
        return 1;
    }
    return 0;
}
"""


BRANCH_FREE = """void store(volatile int* reg, int value)
{
    *reg = value;
}
"""


GOTO_ONLY = """int drain(volatile int* reg)
{
again:
    *reg = 0;
    goto again;
}
"""


class EmptyScanGuardTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def source(self, name, text):
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def run_gate(self, files, changed):
        """main() over `files` with a clang-tidy that reports nothing."""
        silent = subprocess.CompletedProcess([], 0, "", "")
        argv = ["complexity_gate.py",
                "--baseline", str(Path(self.tmp.name) / "baseline.txt")]
        if changed:
            argv += ["--changed", "origin/main"]
        patches = [
            mock.patch.object(sys, "argv", argv),
            mock.patch.object(complexity_gate, "resolve_clang_tidy",
                              return_value=("clang-tidy", "19")),
            mock.patch.object(complexity_gate, "build_compile_db",
                              return_value=(Path("db.json"), files,
                                            {"idf": 0, "host": 0, "tools": 0})),
            mock.patch.object(complexity_gate, "check_main_coverage",
                              return_value=(len(files), [])),
            mock.patch.object(complexity_gate, "changed_files",
                              return_value=files),
            mock.patch.object(complexity_gate, "changed_rel_paths",
                              return_value=set()),
            mock.patch.object(complexity_gate, "scan_vendored",
                              return_value={}),
            mock.patch.object(complexity_gate, "run_clang_tidy_parallel",
                              return_value=silent),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        return complexity_gate.main()

    def test_changed_data_only_file_is_not_a_broken_scan(self):
        files = [self.source("mesh_generated.c", DATA_ONLY)]
        self.assertEqual(self.run_gate(files, changed=True), 0)

    def test_changed_file_with_a_function_still_needs_a_score(self):
        files = [self.source("mesh_generated.c", DATA_ONLY),
                 self.source("pick.c", WITH_FUNCTION)]
        with self.assertRaisesRegex(SystemExit, "ZERO functions"):
            self.run_gate(files, changed=True)

    def test_changed_file_with_only_branch_free_functions_is_not_a_broken_scan(self):
        files = [self.source("store.c", BRANCH_FREE)]
        self.assertEqual(self.run_gate(files, changed=True), 0)

    def test_changed_file_whose_only_control_flow_is_a_goto_still_needs_a_score(self):
        files = [self.source("drain.c", GOTO_ONLY)]
        with self.assertRaisesRegex(SystemExit, "ZERO functions"):
            self.run_gate(files, changed=True)

    def test_full_scan_of_data_only_files_is_still_a_broken_scan(self):
        files = [self.source("mesh_generated.c", DATA_ONLY)]
        with self.assertRaisesRegex(SystemExit, "ZERO functions"):
            self.run_gate(files, changed=False)


class SourceCoverageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.launcher = Path(self.tmp.name)
        patch = mock.patch.object(complexity_gate, "LAUNCHER_DIR", self.launcher)
        patch.start()
        self.addCleanup(patch.stop)

    def source(self, relative):
        path = self.launcher / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return str(path.resolve())

    def test_coverage_uses_vendored_rule_without_hiding_first_party_gaps(self):
        measured = self.source("main/measured.c")
        self.source("packages/testkit/unity/unity.c")
        self.source("packages/testkit/runner/host_main.c")
        total, missing = complexity_gate.check_main_coverage([measured])
        self.assertEqual(total, 2)
        self.assertEqual(missing, ["packages/testkit/runner/host_main.c"])

    def test_package_runners_get_host_commands_without_duplicate_sources(self):
        runner = self.source("packages/testkit/runner/host_main.c")
        nested = self.source("packages/example/runner/nested/entry.c")
        host = self.source("test/host_main.c")
        database = self.launcher / "compile_commands.json"
        database.write_text("[]", encoding="utf-8")
        with mock.patch.object(complexity_gate, "IDF_DB_PATH", database), \
             mock.patch.object(complexity_gate, "BUILD_DIR", self.launcher / "build.tidy"), \
             mock.patch.object(complexity_gate, "find_xtensa_toolchain_root", return_value="/xtensa"), \
             mock.patch.object(complexity_gate, "build_idf_entries", return_value={}), \
             mock.patch.object(complexity_gate, "find_host_toolchain", return_value=("host", "/gcc", None)), \
             mock.patch.object(complexity_gate, "resolve_analysis_clang", return_value="clang"), \
             mock.patch.object(complexity_gate, "run_tests_print", side_effect=[[host, nested], ["-DHOST_TEST"]]):
            db_path, sources, counts = complexity_gate.build_compile_db()
        self.assertEqual(sources, sorted([host, runner, nested]))
        self.assertEqual(counts["host"], 3)
        entries = {entry["file"]: entry for entry in json.loads(db_path.read_text(encoding="utf-8"))}
        self.assertEqual(entries[runner]["arguments"],
                         ["clang", "-DHOST_TEST", "--target=host", "--gcc-toolchain=/gcc",
                          "-Wno-unknown-warning-option", "-Wno-string-plus-int", Path(runner).as_posix()])


class DriverResolutionTest(unittest.TestCase):
    def test_versioned_analyzer_uses_same_major_driver(self):
        tidy = f"/usr/bin/clang-tidy-{pinned_tool.PINNED_MAJOR}"
        driver = tidy.replace("clang-tidy", "clang")
        with mock.patch.object(complexity_gate, "resolve_clang_tidy", return_value=(tidy, pinned_tool.PINNED_MAJOR)), \
             mock.patch.object(pinned_tool, "tool_version", return_value=pinned_tool.PINNED_VERSION) as version:
            self.assertEqual(Path(complexity_gate.resolve_analysis_clang()), Path(driver))
        version.assert_called_once_with(str(Path(driver)))


class DeviceCommandTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.launcher = Path(self.tmp.name)
        self.source = self.launcher / "main/console/console.c"
        self.source.parent.mkdir(parents=True)
        self.source.touch()
        self.db = self.launcher / "compile_commands.json"
        patch = mock.patch.object(complexity_gate, "resolve_analysis_clang",
                                  return_value="/esp/bin/clang")
        patch.start()
        self.addCleanup(patch.stop)
        if hasattr(complexity_gate, "clang_accepts_flag"):
            complexity_gate.clang_accepts_flag.cache_clear()
            self.addCleanup(complexity_gate.clang_accepts_flag.cache_clear)
        for name, value in (("LAUNCHER_DIR", self.launcher), ("IDF_DB_PATH", self.db)):
            patch = mock.patch.object(complexity_gate, name, value)
            patch.start()
            self.addCleanup(patch.stop)

    def entries(self, flags):
        response = self.launcher / "cflags"
        response.write_text(flags, encoding="utf-8")
        self.db.write_text(json.dumps([{
            "file": str(self.source), "directory": str(self.launcher),
            "command": f'"/tool chain/bin/gcc" @"{response}" -Werror -c "{self.source}"',
        }]), encoding="utf-8")
        argv = complexity_gate.build_idf_entries("/tool chain")[str(self.source.resolve())]
        return argv

    def test_posix_quoted_compiler_path_is_unquoted(self):
        compiler = "/tool chain/bin/gcc"
        specs = "-specs=/build dir/picolibc.specs"
        self.db.write_text(json.dumps([{
            "file": str(self.source), "directory": str(self.launcher),
            "command": f"'{compiler}' '{specs}' -c '{self.source}'",
        }]), encoding="utf-8")
        with mock.patch.object(compile_db, "os", SimpleNamespace(name="posix")), \
             mock.patch.object(complexity_gate, "gcc_libc_includes", return_value=()) as probe:
            complexity_gate.build_idf_entries("/tool chain")
        probe.assert_called_once_with(compiler, (specs,), str(self.launcher))

    def test_arguments_expand_relative_response_file(self):
        response = self.launcher / "flags with spaces.rsp"
        response.write_text('"-specs=/build dir/picolibc.specs"', encoding="utf-8")
        self.db.write_text(json.dumps([{
            "file": str(self.source), "directory": str(self.launcher),
            "arguments": ["/tool chain/bin/gcc", "@" + response.name, "-c", str(self.source)],
        }]), encoding="utf-8")
        with mock.patch.object(complexity_gate, "gcc_libc_includes", return_value=()) as probe:
            argv = complexity_gate.build_idf_entries("/tool chain")[str(self.source.resolve())]
        probe.assert_called_once_with("/tool chain/bin/gcc",
                                      ("-specs=/build dir/picolibc.specs",), str(self.launcher))
        self.assertEqual(argv[0], "/tool chain/bin/gcc")
        self.assertEqual(argv[-1], str(self.source))
        self.assertFalse(any(arg.startswith(("@", "-specs=")) for arg in argv))

    def test_rejected_flag_is_dropped(self):
        rejected = subprocess.CompletedProcess([], 1, "", "clang: error: unknown argument: '-fno-ext-dce'")
        with mock.patch.object(complexity_gate.subprocess, "run", return_value=rejected) as probe:
            command = self.entries("-fno-ext-dce")
        self.assertNotIn("-fno-ext-dce", command)
        self.assertIn("-Werror", command)
        probe.assert_called_once()

    def test_unused_flag_is_dropped(self):
        unused = subprocess.CompletedProcess(
            [], 1, "", "clang: error: argument unused during compilation: '-mlongcalls' "
            "[-Werror,-Wunused-command-line-argument]")
        with mock.patch.object(complexity_gate.subprocess, "run", return_value=unused) as probe:
            command = self.entries("-mlongcalls")
        self.assertNotIn("-mlongcalls", command)
        self.assertIn("-Werror", command)
        self.assertIn("-Werror", probe.call_args.args[0])

    def test_accepted_flag_is_kept(self):
        accepted = subprocess.CompletedProcess([], 0, "", "")
        with mock.patch.object(complexity_gate.subprocess, "run", return_value=accepted) as probe:
            command = self.entries("-fno-malloc-dce")
        self.assertIn("-fno-malloc-dce", command)
        probe.assert_called_once()
        self.assertEqual(Path(probe.call_args.args[0][0]), Path("/esp/bin/clang"))
        self.assertEqual(probe.call_args.kwargs["input"], "")
        self.assertIn("-fsyntax-only", probe.call_args.args[0])
        self.assertIn("--target=xtensa-esp-elf", probe.call_args.args[0])

    def test_probe_runs_once_per_flag(self):
        accepted = subprocess.CompletedProcess([], 0, "", "")
        with mock.patch.object(complexity_gate.subprocess, "run", return_value=accepted) as probe:
            for _ in range(2):
                self.entries("-fno-ext-dce -mtext-section-literals -fno-ext-dce")
        self.assertEqual(probe.call_count, 2)

    def test_probe_diagnostics_are_fatal(self):
        for diagnostic in ("clang: error: invalid value 'bad' in '-mcpu=bad'",
                           "<stdin>:1:1: error: expected identifier",
                           "clang: error: unknown argument: '-other-flag'",
                           "clang: error: unknown argument: '-mcpu=bad'" + chr(10) +
                           "<stdin>:1:1: error: expected identifier"):
            with self.subTest(diagnostic=diagnostic):
                result = subprocess.CompletedProcess([], 1, "", diagnostic)
                with mock.patch.object(complexity_gate.subprocess, "run", return_value=result):
                    with self.assertRaisesRegex(SystemExit, "Clang flag probe failed"):
                        self.entries("-mcpu=bad")

    def test_specs_are_replaced_with_gcc_libc_search_order(self):
        with mock.patch.object(complexity_gate, "gcc_libc_includes",
                               return_value=("/tool chain/picolibc/include",)) as probe:
            command = self.entries('"-specs=/build dir/picolibc.specs"')
        probe.assert_called_once_with("/tool chain/bin/gcc",
                                      ("-specs=/build dir/picolibc.specs",), str(self.launcher))
        self.assertNotIn("-specs=", command)
        self.assertIn("-isystem", command)
        self.assertIn("/tool chain/picolibc/include", command)
        self.assertIn("--sysroot=/tool chain/xtensa-esp-elf", command)

    def test_only_entry_padding_attribute_is_neutralized(self):
        command = self.entries("")
        self.assertIn("-Dpatchable_function_entry(...)=", command)
        self.assertNotIn("-Wno-unknown-attributes", command)
        self.assertNotIn("-Wno-error=unknown-attributes", command)


class GccLibcProbeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        complexity_gate.gcc_libc_includes.cache_clear()
        self.addCleanup(complexity_gate.gcc_libc_includes.cache_clear)

    def test_probe_preserves_libc_order_and_omits_private_headers(self):
        paths = [self.root / name for name in ("picolibc/include", "gcc/include", "newlib/include")]
        for path in paths:
            path.mkdir(parents=True)
            (path / ("stddef.h" if path == paths[1] else complexity_gate.LIBC_HEADER)).touch()
        stderr = (complexity_gate.GCC_INCLUDE_START + "\n" +
                  "\n".join(" " + str(path) for path in paths) + "\n" + complexity_gate.GCC_INCLUDE_END)
        result = subprocess.CompletedProcess([], 0, "", stderr)
        with mock.patch.object(complexity_gate.subprocess, "run", return_value=result) as probe:
            includes = complexity_gate.gcc_libc_includes("gcc", ("-specs=libc.specs",), str(self.root))
            self.assertEqual(includes, (str(paths[0].resolve()), str(paths[2].resolve())))
            self.assertEqual(includes, complexity_gate.gcc_libc_includes(
                "gcc", ("-specs=libc.specs",), str(self.root)))
        probe.assert_called_once()
        self.assertEqual(probe.call_args.args[0],
                         ["gcc", "-specs=libc.specs", "-E", "-v", "-x", "c", "-"])
        self.assertEqual(probe.call_args.kwargs["timeout"], complexity_gate.GCC_PROBE_TIMEOUT_SECONDS)
        self.assertEqual(probe.call_args.kwargs["input"], "")
        self.assertEqual(probe.call_args.kwargs["env"]["LC_ALL"], "C")

    def test_invalid_gcc_probe_is_fatal(self):
        cases = ((1, "missing specs", "GCC libc include probe failed"),
                 (0, "no search list", "no libc include directories"))
        for status, stderr, message in cases:
            with self.subTest(status=status):
                result = subprocess.CompletedProcess([], status, "", stderr)
                with mock.patch.object(complexity_gate.subprocess, "run", return_value=result):
                    with self.assertRaisesRegex(SystemExit, message):
                        complexity_gate.gcc_libc_includes("gcc", ("-specs=libc",), str(self.root))

    def test_real_parse_errors_remain_fatal(self):
        errors = complexity_gate.find_parse_errors(
            "error: unknown argument: '-funknown-option' [clang-diagnostic-error]\n"
            "/app/source.c:1:2: error: use of undeclared identifier 'missing' [clang-diagnostic-error]")
        self.assertEqual(len(errors), 2)


if __name__ == "__main__":
    unittest.main()
