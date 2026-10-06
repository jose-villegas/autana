"""Regression tests for scripts/gates/check_device_access.py."""
import pathlib
import subprocess
import sys
import tempfile
import unittest
import tree

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import check_device_access  # noqa: E402


class DeviceAccessTest(unittest.TestCase):
    def test_a_pyserial_open_outside_scripts_device_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "launcher/tools/capture.py",
                      "import serial\n\nport = serial.Serial('COM5', 115200)\n")
            tree.commit(root, "launcher")
            openers = check_device_access.check(root)
        self.assertEqual(len(openers), 1)
        self.assertEqual(openers[0].path, "launcher/tools/capture.py")

    def test_the_same_opener_inside_scripts_device_is_exempt(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "scripts/device/device.py",
                      "import serial\n\nport = serial.Serial('COM5', 115200)\n")
            tree.commit(root, "scripts")
            openers = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_the_gate_itself_is_exempt(self):
        # A gate's own source and tests describe and exercise these exact
        # patterns as data, real string literals, matching the ones that
        # once tripped this check on its own docstring and test fixtures.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "scripts/gates/check_device_access.py",
                      'IDF_MONITOR_RE = "idf.py monitor"\n'
                      'ESPTOOL = [\'"-m", "esptool"\']\n')
            tree.write(root, "scripts/gates/tests/test_check_device_access.py",
                      'FIXTURE = \'cmd = [python, "-m", "esptool", "flash"]\'\n')
            tree.commit(root, "scripts")
            openers = check_device_access.check(root)
        self.assertEqual(openers, [])

    def openers(self, path, text):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, path, text)
            tree.commit(root, path)
            openers = check_device_access.check(root)
        return [v.line for v in openers]

    def test_a_flash_outside_scripts_device_is_flagged_whatever_checks_the_lock(self):
        self.assertEqual(self.openers(
            "launcher/tools/build/build.sh",
            '#!/bin/sh\n'
            'python "$DIR/scripts/device/device.py" check-token --token "$T" || exit 1\n'
            'idf -B "$BUILD_DIR" -p "$COM_PORT" flash\n'), [3])

    def test_a_shell_merge_bin_is_not_flagged(self):
        self.assertEqual(self.openers(
            "launcher/test/merge.sh",
            '#!/bin/sh\nesptool.py --chip esp32s3 merge-bin -o flash.bin 0x0 boot.bin\n'), [])

    def test_a_line_naming_merge_bin_but_running_write_flash_is_flagged(self):
        for line in ('esptool -p COM5 write_flash 0x0 image.bin  # merge_bin\n',
                     'esptool --chip esp32s3 merge_bin -o a.bin && esptool -p COM5 write_flash 0x0 a.bin\n'):
            with self.subTest(line=line):
                self.assertEqual(self.openers("launcher/tools/flash_it.sh", "#!/bin/sh\n" + line), [2])

    def test_a_filename_containing_an_offline_name_is_flagged(self):
        self.assertEqual(self.openers(
            "launcher/tools/flash_it.sh",
            '#!/bin/sh\nesptool -p COM5 write_flash 0x0 merge_bin_image_info.bin\n'), [2])

    def test_merge_bin_on_the_line_after_the_esptool_module_is_not_flagged(self):
        self.assertEqual(self.openers(
            "launcher/test/qemu_merge.py",
            'cmd = [python, "-m", "esptool",\n'
            '       "merge_bin", "-o", out_path]\n'), [])

    def test_a_python_write_flash_split_across_lines_is_flagged(self):
        self.assertEqual(self.openers(
            "launcher/tools/flash_it.py",
            'cmd = [python, "-m", "esptool", "--port", port,\n'
            '       "write_flash", "0x0", "merge_bin.bin"]\n'), [1])

    def test_an_esptool_image_merge_is_not_flagged(self):
        # QEMU boots a merged image file; merge_bin never opens a port.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "launcher/test/qemu_run.py",
                      'import socket\n\n'
                      'cmd = [python, "-m", "esptool", "--chip", "esp32s3", "merge_bin",\n'
                      '       "-o", out_path]\n'
                      'sock = socket.create_connection(("127.0.0.1", port), 1.0)\n')
            tree.commit(root, "launcher")
            openers = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_a_bare_serial_call_with_no_import_is_not_flagged(self):
        # Some other module's own Serial(...) call; the gate only flags
        # pyserial's, which needs `import serial` in the same file.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "launcher/tools/example.py",
                      "def Serial(x):\n    return x\n\nSerial(1)\n")
            tree.commit(root, "launcher")
            openers = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_an_esptool_module_invocation_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "launcher/tools/flash_it.py",
                      'cmd = [python, "-m", "esptool", "--chip", "esp32s3", "flash"]\n')
            tree.commit(root, "launcher")
            openers = check_device_access.check(root)
        self.assertEqual(len(openers), 1)
        self.assertIn("esptool", openers[0].reason)

    def test_an_esptool_mention_inside_an_echo_string_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "launcher/tools/build_it.sh",
                      '#!/bin/sh\necho "=== letting esptool pick the port ==="\n')
            tree.commit(root, "launcher")
            openers = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_an_idf_flash_invocation_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "launcher/tools/rogue_flash.sh",
                      '#!/bin/sh\nidf -B build -p "$COM_PORT" flash\n')
            tree.commit(root, "launcher")
            openers = check_device_access.check(root)
        self.assertEqual(len(openers), 1)
        self.assertIn("flash", openers[0].reason)

    def test_an_idf_flash_mention_inside_an_echo_string_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "launcher/tools/guidance.sh",
                      '#!/bin/sh\necho "Flash with: idf.py -B build -p COM3 flash"\n')
            tree.commit(root, "launcher")
            openers = check_device_access.check(root)
        self.assertEqual(openers, [])

    def test_an_idf_monitor_script_invocation_is_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "monitor.sh",
                      '#!/bin/sh\nexec "$PY" "$IDF_PATH/tools/idf_monitor.py" -p "$PORT"\n')
            tree.commit(root, "monitor.sh")
            openers = check_device_access.check(root)
        self.assertEqual(len(openers), 1)
        self.assertIn("idf_monitor", openers[0].reason)

    def test_idf_monitor_named_in_prose_is_not_flagged(self):
        # screenshot.py's own docstring used to trip this before the regex
        # was narrowed to an actual invocation shape.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            tree.write(root, "launcher/tools/device/screenshot.py",
                      '"""Reads the response back out of the same stream idf_monitor\n'
                      'would otherwise be showing as logs.\n"""\n')
            tree.commit(root, "launcher")
            openers = check_device_access.check(root)
        self.assertEqual(openers, [])


class MainTest(unittest.TestCase):
    def test_exits_nonzero_and_prints_a_summary_line(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
            subprocess.run(git + ["init", "-q"], cwd=root, check=True)
            (root / "tools").mkdir()
            (root / "tools" / "capture.py").write_text("import serial\nserial.Serial('COM5')\n",
                                                       encoding="utf-8")
            subprocess.run(git + ["add", "tools"], cwd=root, check=True)
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
