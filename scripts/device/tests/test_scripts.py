"""Every shell script on the build and flash path parses: `bash -n`, plus
shellcheck's errors when shellcheck is installed. A line that parses but
runs wrong is the end-to-end tests' job (FlashImageScriptTests). It also runs
build.sh with the ESP-IDF stubbed."""

import os
import re
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
import fake_esp_idf  # noqa: E402

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


class BuildShConfigureTests(unittest.TestCase):
    """build.sh run from a copy of its folder with the ESP-IDF stubbed:
    --configure reconfigures with the arguments the build would get, and
    still checks the generated symbols."""

    SDKCONFIGS = {"release": "CONFIG_LAUNCHER_RELEASE=y",
                  "dev": "CONFIG_LAUNCHER_DEVELOPMENT=y",
                  "diag": "CONFIG_LAUNCHER_DEVELOPMENT=y\nCONFIG_LAUNCHER_SELFTEST=y"}
    DIRECTORIES = {"release": "build", "dev": "build.dev", "diag": "build.diag"}
    FLAGS = {"release": [], "dev": ["--dev"], "diag": ["--diag"]}

    # Logs its arguments and copies in the sdkconfig the test hands it; for
    # `build` it also leaves an image.
    FAKE_IDF_PY = """import os, shutil, sys
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(" ".join(args) + "\\n")
directory = args[args.index("-B") + 1]
os.makedirs(directory, exist_ok=True)
shutil.copy(os.environ["FAKE_SDKCONFIG"], os.path.join(directory, "sdkconfig"))
if args[-1] == "build":
    for image in ("launcher.elf", "launcher.bin"):
        open(os.path.join(directory, image), "w").write("image")
"""

    def setUp(self):
        try:
            self.bash = device.git_bash()
        except RuntimeError as error:
            self.skipTest(str(error))

    def run_build_sh(self, flags, sdkconfig, previous_idf=None):
        """Returns the result, the fake's logged argv lines and the files
        left in the launcher folder."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tools = root / "launcher" / "tools" / "build"
            tools.mkdir(parents=True)
            (root / "scripts/lib").mkdir(parents=True)
            for name in ("python.sh", "autana_config.py"):
                shutil.copy(ENGINE / "scripts/lib" / name, root / "scripts/lib" / name)
            (root / "autana.local.toml").write_text(f"build_root = '{(root / 'b').as_posix()}'\n")
            for name in ("build.sh", "idf.sh", "idf_shim.bat", "idf_variant.sh", "write_build_id.sh", "espressif.py", "build_paths.py"):
                shutil.copy(ENGINE / "launcher/tools/build" / name, tools / name)
            shutil.copy(ENGINE / "scripts/quiet.sh", root / "scripts" / "quiet.sh")
            export = fake_esp_idf.stub_esp_idf(root / "stubs", self.FAKE_IDF_PY)
            (root / "sdkconfig").write_text(sdkconfig)
            if previous_idf:
                sys.path.insert(0, str(ENGINE / "launcher/tools/build"))
                import build_paths
                build = build_paths.prepare(root / "launcher", "build.dev")
                selected = {"commit": None, "path": str(export.parent.resolve())}
                build_paths.stamp(build, selected if previous_idf == "matching" else {**selected, "commit": "old"})
                (build / "sentinel").write_text("configured output")
            environment = dict(os.environ, FAKE_LOG=(root / "log").as_posix(),
                               FAKE_SDKCONFIG=str(root / "sdkconfig"))
            result = subprocess.run([self.bash, (tools / "build.sh").as_posix(), *flags, export.as_posix()],
                                    capture_output=True, text=True, timeout=120,
                                    stdin=subprocess.DEVNULL, env=environment)
            log = (root / "log").read_text().splitlines() if (root / "log").exists() else []
            files = {name + "/" + path.relative_to(root / "launcher" / name).as_posix()
                     for name in self.DIRECTORIES.values() if (root / "launcher" / name).exists()
                     for path in (root / "launcher" / name).rglob("*") if path.is_file()}
            log = [re.sub(r"(?:[A-Za-z]:)?[^ ]*/b/[^ ]*/(build(?:\.[a-z]+)?)(/sdkconfig)?",
                          r"\1\2", line) for line in log]
        return result, log, files

    def test_idf_identity_reconfigures_stale_output_only(self):
        stale, _, stale_files = self.run_build_sh(["--dev", "--configure"], self.SDKCONFIGS["dev"], "stale")
        self.assertEqual(stale.returncode, 0, stale.stdout + stale.stderr)
        self.assertIn("reconfiguring from scratch", stale.stdout + stale.stderr)
        self.assertNotIn("build.dev/sentinel", stale_files)
        matched, _, matched_files = self.run_build_sh(["--dev", "--configure"], self.SDKCONFIGS["dev"], "matching")
        self.assertEqual(matched.returncode, 0, matched.stdout + matched.stderr)
        self.assertNotIn("reconfiguring from scratch", matched.stdout + matched.stderr)
        self.assertIn("build.dev/sentinel", matched_files)

    def test_configure_reconfigures_with_the_builds_arguments(self):
        for variant, sdkconfig in self.SDKCONFIGS.items():
            directory = self.DIRECTORIES[variant]
            with self.subTest(variant=variant):
                built, built_log, built_files = self.run_build_sh(self.FLAGS[variant], sdkconfig)
                self.assertEqual(built.returncode, 0, built.stderr + built.stdout)
                self.assertTrue(built_log[-1].endswith(" build"), built_log)
                self.assertIn(directory + "/build_id.txt", built_files)
                configured, log, files = self.run_build_sh([*self.FLAGS[variant], "--configure"], sdkconfig)
                self.assertEqual(configured.returncode, 0, configured.stderr + configured.stdout)
                self.assertIn("configured, nothing built", configured.stdout)
                self.assertEqual(len(log), 1, log)
                self.assertTrue(log[0].endswith(" reconfigure"), log)
                self.assertIn("-B " + directory + " ", log[0])
                self.assertEqual(log[0].rsplit(" ", 1)[0], built_log[0].rsplit(" ", 1)[0])
                self.assertIn(directory + "/sdkconfig", files)
                self.assertNotIn(directory + "/build_id.txt", files)
                self.assertNotIn(directory + "/launcher.bin", files)

    def test_configure_still_refuses_a_wrong_sdkconfig(self):
        for sdkconfig, complaint in (
                ("CONFIG_LAUNCHER_SELFTEST=y", "CONFIG_LAUNCHER_DEVELOPMENT is not set"),
                ("CONFIG_LAUNCHER_DEVELOPMENT=y\nCONFIG_LAUNCHER_QEMU=y", "CONFIG_LAUNCHER_QEMU is set")):
            with self.subTest(sdkconfig=sdkconfig):
                result, _, _ = self.run_build_sh(["--dev", "--configure"], sdkconfig)
                self.assertEqual(result.returncode, 1)
                self.assertIn(complaint, result.stdout + result.stderr)

    def test_configure_keeps_the_options_a_build_gets(self):
        for flags, sdkconfig, expected in (
                (["--dev", "--layout-seed", "7", "--hot-tunables"], self.SDKCONFIGS["dev"],
                 ("LAUNCHER_LAYOUT_SEED=7", "TUNE_HOT_LIVE=1")),
                (["--diag", "--perf-scope"], self.SDKCONFIGS["diag"] + "\nCONFIG_LAUNCHER_SELFTEST_SCOPE_PERF=y",
                 ("sdkconfig.defaults.diag_perf",))):
            with self.subTest(flags=flags):
                built, built_log, _ = self.run_build_sh(flags, sdkconfig)
                configured, log, _ = self.run_build_sh([*flags, "--configure"], sdkconfig)
                self.assertEqual((built.returncode, configured.returncode), (0, 0),
                                 built.stdout + configured.stdout)
                for text in expected:
                    self.assertIn(text, log[0])
                self.assertEqual(log[0].rsplit(" ", 1)[0], built_log[0].rsplit(" ", 1)[0])


if __name__ == "__main__":
    unittest.main()
