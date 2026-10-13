"""Every shell script on the build and flash path parses: `bash -n`, plus
shellcheck's errors when shellcheck is installed. A line that parses but
runs wrong is the end-to-end tests' job (FlashImageScriptTests)."""

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
    """idf_variant_build with `idf` stubbed to record its arguments and write
    the sdkconfig a variant would get: --configure reconfigures, with the
    defaults the build uses, and still checks the generated symbols."""

    def run_variant(self, variant, options, sdkconfig):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "wanted").write_text(sdkconfig)
            script = ('. "$SCRIPT"; idf() { echo "$*" > "$ROOT/idf.args"; mkdir -p "$ROOT/b";'
                      ' cp "$ROOT/wanted" "$ROOT/b/sdkconfig"; }; idf_variant_build "$ROOT" "$1" b $2')
            result = subprocess.run(
                ["sh", "-c", script, "sh", variant, options], capture_output=True, text=True,
                timeout=60, env=dict(os.environ, ROOT=root.as_posix(), SCRIPT=(
                    ENGINE / "launcher/tools/build/idf_variant.sh").as_posix()))
            args = (root / "idf.args").read_text()
        return result, args

    def test_configure_reconfigures_with_the_builds_defaults(self):
        built, build_args = self.run_variant("dev", "", "CONFIG_LAUNCHER_DEVELOPMENT=y")
        configured, args = self.run_variant("dev", "--configure", "CONFIG_LAUNCHER_DEVELOPMENT=y")
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

    def test_build_sh_skips_the_build_id_when_configuring(self):
        text = (ENGINE / "launcher/tools/build/build.sh").read_text()
        configured = text.index('"$CONFIGURE" -eq 1 ]; then\n    echo')
        self.assertLess(configured, text.index("write_build_id.sh"))
        self.assertIn("exit 0", text[configured:text.index("write_build_id.sh")])


if __name__ == "__main__":
    unittest.main()
