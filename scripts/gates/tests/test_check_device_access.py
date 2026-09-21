"""Regression tests for scripts/gates/check_device_access.py."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import check_device_access  # noqa: E402


class DeviceAccessTest(unittest.TestCase):
    def write(self, root, path, text):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def commit(self, root, *paths):
        git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
        subprocess.run(git + ["init", "-q"], cwd=root, check=True)
        subprocess.run(git + ["add", *paths], cwd=root, check=True)
        subprocess.run(git + ["commit", "-qm", "fixture"], cwd=root, check=True)

    def test_a_pyserial_open_outside_scripts_device_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/tools/capture.py",
                      "import serial\n\nport = serial.Serial('COM5', 115200)\n")
            self.commit(root, "launcher")
            openers, retired = check_device_access.check(root)
        self.assertEqual(retired, [])
        self.assertEqual(len(openers), 1)
        self.assertEqual(openers[0].path, "launcher/tools/capture.py")

    def test_the_same_opener_inside_scripts_device_is_exempt(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/device/device.py",
                      "import serial\n\nport = serial.Serial('COM5', 115200)\n")
            self.commit(root, "scripts")
            openers, _ = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_the_gate_itself_is_exempt(self):
        # A gate's own source and tests describe and exercise these exact
        # patterns as data - real string literals, matching the ones that
        # once tripped this check on its own docstring and test fixtures.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/gates/check_device_access.py",
                      'IDF_MONITOR_RE = "idf.py monitor"\n'
                      'ESPTOOL = [\'"-m", "esptool"\']\n')
            self.write(root, "scripts/gates/tests/test_check_device_access.py",
                      'FIXTURE = \'cmd = [python, "-m", "esptool", "flash"]\'\n')
            self.commit(root, "scripts")
            openers, _ = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_an_allowlisted_path_is_exempt(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/test/qemu_run.py",
                      "import serial\n\nport = serial.Serial('COM5', 115200)\n")
            self.write(root, "scripts/gates/device_access_allowlist.txt",
                      "launcher/test/qemu_run.py\tQEMU, not the board.\n")
            self.commit(root, "launcher", "scripts")
            openers, _ = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_a_bare_serial_call_with_no_import_is_not_flagged(self):
        # Some other module's own Serial(...) call - the gate only flags
        # pyserial's, which needs `import serial` in the same file.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/tools/example.py",
                      "def Serial(x):\n    return x\n\nSerial(1)\n")
            self.commit(root, "launcher")
            openers, _ = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_an_esptool_module_invocation_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/tools/flash_it.py",
                      'cmd = [python, "-m", "esptool", "--chip", "esp32s3", "flash"]\n')
            self.commit(root, "launcher")
            openers, _ = check_device_access.check(root)
        self.assertEqual(len(openers), 1)
        self.assertIn("esptool", openers[0].reason)

    def test_an_esptool_mention_inside_an_echo_string_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/tools/build_it.sh",
                      '#!/bin/sh\necho "=== letting esptool pick the port ==="\n')
            self.commit(root, "launcher")
            openers, _ = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_an_idf_monitor_script_invocation_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "monitor.sh",
                      '#!/bin/sh\nexec "$PY" "$IDF_PATH/tools/idf_monitor.py" -p "$PORT"\n')
            self.commit(root, "monitor.sh")
            openers, _ = check_device_access.check(root)
        self.assertEqual(len(openers), 1)
        self.assertIn("idf_monitor", openers[0].reason)

    def test_idf_monitor_named_in_prose_is_not_flagged(self):
        # screenshot.py's own docstring used to trip this before the regex
        # was narrowed to an actual invocation shape.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/tools/screenshot.py",
                      '"""Reads the response back out of the same stream idf_monitor\n'
                      'would otherwise be showing as logs.\n"""\n')
            self.commit(root, "launcher")
            openers, _ = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_a_doc_naming_a_retired_script_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md",
                      "Capture the screen with `./launcher/tools/screenshot.sh`.\n")
            self.commit(root, "docs")
            _, retired = check_device_access.check(root)
        self.assertEqual(len(retired), 1)
        self.assertIn("screenshot.sh", retired[0].reason)

    def test_a_doc_naming_the_replacement_command_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "docs/Guide.md", "Capture the screen with `autana screenshot`.\n")
            self.commit(root, "docs")
            _, retired = check_device_access.check(root)
        self.assertEqual(retired, [])

    def test_every_retired_script_name_is_individually_caught(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            lines = "\n".join(f"- `{name}`" for name in check_device_access.RETIRED_SCRIPTS)
            self.write(root, "docs/Guide.md", lines + "\n")
            self.commit(root, "docs")
            _, retired = check_device_access.check(root)
        self.assertEqual(len(retired), len(check_device_access.RETIRED_SCRIPTS))


class MainTest(unittest.TestCase):
    def test_exits_nonzero_and_prints_a_summary_line(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
            subprocess.run(git + ["init", "-q"], cwd=root, check=True)
            (root / "docs").mkdir()
            (root / "docs" / "Guide.md").write_text("`monitor.sh`\n", encoding="utf-8")
            subprocess.run(git + ["add", "docs"], cwd=root, check=True)
            subprocess.run(git + ["commit", "-qm", "fixture"], cwd=root, check=True)
            import os
            cwd = os.getcwd()
            try:
                os.chdir(root)
                code = check_device_access.main([])
            finally:
                os.chdir(cwd)
        self.assertEqual(code, 1)

    def test_rejects_arguments(self):
        self.assertEqual(check_device_access.main(["--bogus"]), 2)


if __name__ == "__main__":
    unittest.main()
