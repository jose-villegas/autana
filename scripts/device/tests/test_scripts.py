"""Every shell script on the build and flash path parses: `bash -n`, plus
shellcheck's errors when shellcheck is installed. A line that parses but
runs wrong is the end-to-end tests' job (FlashImageScriptTests). It also runs
idf_variant_build and build.sh with the ESP-IDF stubbed."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[1]
ENGINE = DEVICE.parents[1]
sys.path.insert(0, str(DEVICE))
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)
import device  # noqa: E402

FOLDERS = ("scripts/device", "launcher/tools/build")


def scripts():
    """Tracked and untracked alike: a new script is checked before its first
    commit."""
    listed = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard",
                             "--", *(f"{folder}/*.sh" for folder in FOLDERS)],
                            cwd=ENGINE, capture_output=True, text=True, check=True)
    return sorted({ENGINE / line for line in listed.stdout.splitlines() if line})


class ScriptSyntaxTests(unittest.TestCase):
    def test_there_are_scripts_to_check(self):
        names = {path.name for path in scripts()}
        self.assertTrue({"flash_image.sh", "build.sh", "idf.sh"} <= names, names)

    def test_every_script_parses(self):
        try:
            bash = device.git_bash()
        except RuntimeError as error:
            self.skipTest(str(error))
        for path in scripts():
            with self.subTest(script=path.relative_to(ENGINE).as_posix()):
                result = subprocess.run([bash, "-n", path.as_posix()], capture_output=True,
                                        text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_shellcheck_finds_no_error(self):
        shellcheck = shutil.which("shellcheck")
        if shellcheck is None:
            self.skipTest("shellcheck is not installed")
        result = subprocess.run([shellcheck, "-S", "error",
                                 *(path.relative_to(ENGINE).as_posix() for path in scripts())],
                                cwd=ENGINE, capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])


class IdfVariantConfigureTests(unittest.TestCase):
    """idf_variant_build and build.sh with the ESP-IDF stubbed: --configure
    reconfigures with the arguments the build would get, and still checks the
    generated symbols."""

    SDKCONFIGS = {"release": "CONFIG_LAUNCHER_RELEASE=y",
                  "dev": "CONFIG_LAUNCHER_DEVELOPMENT=y",
                  "diag": "CONFIG_LAUNCHER_DEVELOPMENT=y\nCONFIG_LAUNCHER_SELFTEST=y"}

    FAKE_IDF_PY = """#!/bin/sh
echo "$*" >> "$FAKE_LOG"
for last; do :; done
while [ "$1" != -B ]; do shift; done
mkdir -p "$2"
case "$2" in
    build.dev) echo CONFIG_LAUNCHER_DEVELOPMENT=y ;;
    build.diag) echo CONFIG_LAUNCHER_DEVELOPMENT=y; echo CONFIG_LAUNCHER_SELFTEST=y ;;
    *) echo CONFIG_LAUNCHER_RELEASE=y ;;
esac > "$2/sdkconfig"
if [ "$last" = build ]; then echo elf > "$2/launcher.elf"; echo bin > "$2/launcher.bin"; fi
"""

    def setUp(self):
        try:
            self.bash = device.git_bash()
        except RuntimeError as error:
            self.skipTest(str(error))

    def run_variant(self, variant, options, sdkconfig):
        """idf_variant_build, its `idf` a function that records its arguments
        and copies in the sdkconfig this test hands it."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "wanted").write_text(sdkconfig)
            script = ('. "$SCRIPT"; idf() { echo "$*" > "$ROOT/idf.args"; mkdir -p "$ROOT/b";'
                      ' cp "$ROOT/wanted" "$ROOT/b/sdkconfig"; }; idf_variant_build "$ROOT" "$1" b $2')
            result = subprocess.run(
                [self.bash, "-c", script, "bash", variant, options], capture_output=True,
                text=True, timeout=60, env=dict(os.environ, ROOT=root.as_posix(), SCRIPT=(
                    ENGINE / "launcher/tools/build/idf_variant.sh").as_posix()))
            args = (root / "idf.args").read_text()
        return result, args

    def run_build_sh(self, flags):
        """build.sh itself, run from a copy of its folder with an export
        script that puts a fake idf.py on PATH. Returns the result, the
        fake's logged argv lines and the files left in the launcher folder."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tools = root / "launcher" / "tools" / "build"
            tools.mkdir(parents=True)
            (root / "scripts").mkdir()
            for name in ("build.sh", "idf.sh", "idf_shim.bat", "idf_variant.sh", "write_build_id.sh"):
                shutil.copy(ENGINE / "launcher/tools/build" / name, tools / name)
            shutil.copy(ENGINE / "scripts/quiet.sh", root / "scripts" / "quiet.sh")
            (root / "bin").mkdir()
            fake = root / "bin" / ("fake_idf" if os.name == "nt" else "idf.py")
            fake.write_text(self.FAKE_IDF_PY, newline="\n")
            fake.chmod(0o755)
            export = root / "export.sh"
            export.write_text('PATH="$FAKE_BIN:$PATH"\n', newline="\n")
            if os.name == "nt":
                # Git Bash always sets MSYSTEM, so build.sh goes through the real
                # idf_shim.bat; cmd finds idf.py.bat for `idf.py` and hands the fake to bash.
                export = root / "export.bat"
                export.write_text("@set PATH=%FAKE_BIN%;%PATH%\r\n", newline="")
                (root / "bin" / "idf.py.bat").write_text(
                    '@"%FAKE_BASH%" "%FAKE_BIN%\\fake_idf" %*\r\n', newline="")
            environment = dict(os.environ, FAKE_BASH=self.bash, FAKE_BIN=str(root / "bin"),
                               FAKE_LOG=(root / "log").as_posix())
            result = subprocess.run(
                [self.bash, (tools / "build.sh").as_posix(), *flags, export.as_posix()],
                capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL, env=environment)
            log = (root / "log").read_text().splitlines() if (root / "log").exists() else []
            files = {path.relative_to(root / "launcher").as_posix()
                     for path in (root / "launcher").rglob("*") if path.is_file()}
        return result, log, files

    def test_configure_reconfigures_with_the_builds_defaults(self):
        for variant, sdkconfig in self.SDKCONFIGS.items():
            with self.subTest(variant=variant):
                built, build_args = self.run_variant(variant, "", sdkconfig)
                configured, args = self.run_variant(variant, "--configure", sdkconfig)
                self.assertEqual(built.returncode, 1, "a build with no launcher.bin must still fail")
                self.assertEqual(configured.returncode, 0, configured.stderr)
                self.assertTrue(args.rstrip().endswith("reconfigure"), args)
                self.assertTrue(build_args.rstrip().endswith("build"), build_args)
                self.assertEqual(args.rsplit(" ", 1)[0], build_args.rsplit(" ", 1)[0])
                self.assertIn("Configuring b", configured.stdout)

    def test_configure_still_refuses_a_wrong_sdkconfig(self):
        result, _ = self.run_variant("dev", "--configure", "CONFIG_LAUNCHER_SELFTEST=y")
        self.assertEqual(result.returncode, 1)
        self.assertIn("CONFIG_LAUNCHER_DEVELOPMENT is not set", result.stderr)

    def test_configure_refuses_a_forbidden_symbol(self):
        result, _ = self.run_variant(
            "dev", "--configure", "CONFIG_LAUNCHER_DEVELOPMENT=y\nCONFIG_LAUNCHER_QEMU=y")
        self.assertEqual(result.returncode, 1)
        self.assertIn("CONFIG_LAUNCHER_QEMU is set", result.stderr)

    def test_build_sh_configures_without_building(self):
        for flags, directory in (([], "build"), (["--dev"], "build.dev"), (["--diag"], "build.diag")):
            with self.subTest(flags=flags):
                built, built_log, built_files = self.run_build_sh(flags)
                self.assertEqual(built.returncode, 0, built.stderr + built.stdout)
                self.assertTrue(built_log[0].endswith(" build"), built_log)
                self.assertIn(directory + "/build_id.txt", built_files)
                configured, log, files = self.run_build_sh([*flags, "--configure"])
                self.assertEqual(configured.returncode, 0, configured.stderr + configured.stdout)
                self.assertEqual(len(log), 1, log)
                self.assertTrue(log[0].endswith(" reconfigure"), log)
                self.assertIn("-B " + directory + " ", log[0])
                self.assertEqual(log[0].rsplit(" ", 1)[0], built_log[0].rsplit(" ", 1)[0])
                self.assertIn(directory + "/sdkconfig", files)
                self.assertNotIn(directory + "/build_id.txt", files)
                self.assertNotIn(directory + "/launcher.bin", files)

    def test_configure_keeps_the_options_a_build_gets(self):
        for flags, expected in ((["--dev", "--layout-seed", "7", "--hot-tunables"],
                                 ("LAUNCHER_LAYOUT_SEED=7", "TUNE_HOT_LIVE=1")),
                                (["--diag", "--perf-scope"], ("sdkconfig.defaults.diag_perf",))):
            with self.subTest(flags=flags):
                _, built_log, _ = self.run_build_sh(flags)
                _, log, _ = self.run_build_sh([*flags, "--configure"])
                for text in expected:
                    self.assertIn(text, log[0])
                self.assertEqual(log[0].rsplit(" ", 1)[0], built_log[0].rsplit(" ", 1)[0])


if __name__ == "__main__":
    unittest.main()
