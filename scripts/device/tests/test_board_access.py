import ast
import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock
import sys

DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE))
import device
import device_lock


class FlashProofTests(unittest.TestCase):
    def test_image_verification_uses_the_app_image_from_flash_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "launcher.bin").write_bytes(b"image")
            (build / "flasher_args.json").write_text(
                '{"flash_files":{"0x0":"bootloader/bootloader.bin",'
                '"0x10000":"launcher.bin"}}', encoding="utf-8")
            with mock.patch.object(device, "require_port_lock"), \
                 mock.patch.object(device, "find_port", return_value="COM5"), \
                 mock.patch.object(device, "open_serial"), \
                 mock.patch.object(device.subprocess, "run") as run:
                device.verify_flashed_image("COM5", build)
            command = run.call_args.args[0]
            self.assertIn("verify_flash", command)
            self.assertIn("0x10000", command)
            self.assertIn(str(build / "launcher.bin"), command)

    def test_missing_image_is_not_reported_as_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(RuntimeError):
                device.verify_flashed_image("COM5", Path(directory))

    def test_verification_waits_for_usb_to_reenumerate(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "launcher.bin").write_bytes(b"image")
            (build / "flasher_args.json").write_text(
                '{"flash_files":{"0x10000":"launcher.bin"}}', encoding="utf-8")
            ports = iter([RuntimeError("USB absent"), "COM7"])

            def find_port():
                found = next(ports)
                if isinstance(found, Exception):
                    raise found
                return found

            with mock.patch.object(device, "require_port_lock"), \
                 mock.patch.object(device, "find_port", side_effect=find_port), \
                 mock.patch.object(device, "open_serial"), \
                 mock.patch.object(device.time, "sleep"), \
                 mock.patch.object(device.subprocess, "run") as run:
                device.verify_flashed_image("COM5", build)
            command = run.call_args.args[0]
            self.assertEqual(command[command.index("-p") + 1], "COM7")


class LockBoundaryTests(unittest.TestCase):
    def test_every_port_primitive_rejects_an_unguarded_call(self):
        tree = ast.parse((DEVICE / "device.py").read_text(encoding="utf-8"))
        primitive_names = {
            node.name for node in tree.body if isinstance(node, ast.FunctionDef)
            and (any(isinstance(call, ast.Call) and isinstance(call.func, ast.Attribute)
                     and call.func.attr == "Serial" for call in ast.walk(node))
                 or (any(isinstance(arg, ast.Constant) and arg.value == "esptool"
                         for arg in ast.walk(node))
                     and any(isinstance(call, ast.Call) and isinstance(call.func, ast.Attribute)
                             and call.func.attr == "run" for call in ast.walk(node))))
        }
        self.assertIn("open_serial", primitive_names)
        self.assertIn("reset", primitive_names)
        for name in primitive_names:
            with self.subTest(name=name), mock.patch.object(device, "require_port_lock",
                                                       side_effect=RuntimeError("no lock")) as guard:
                try:
                    if name == "verify_flashed_image":
                        getattr(device, name)("COM5", Path("unused"))
                    else:
                        getattr(device, name)("COM5")
                except RuntimeError as error:
                    self.assertEqual(str(error), "no lock")
                guard.assert_called()

    def test_flash_script_checks_the_live_token_before_opening_the_port(self):
        script = (DEVICE.parents[1] / "launcher" / "tools" / "build" / "build_flash.sh")
        text = script.read_text(encoding="utf-8")
        self.assertLess(text.index("check-token"), text.index('idf -B "$BUILD_DIR" -p "$COM_PORT" flash'))

    def test_a_guessed_token_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            store = device_lock.LockStore(directory)
            self.assertFalse(store.check_token("COM5", "guessed"))
            held = store.acquire("COM5", "Alice", "flash")
            self.assertFalse(store.check_token("COM5", "guessed"))
            self.assertTrue(store.check_token("COM5", held["token"]))


class QueueVisibilityTests(unittest.TestCase):
    def test_status_uses_command_durations_for_ordered_start_estimates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = root / "records"
            records.mkdir()
            (records / "index.jsonl").write_text(
                '{"command":"flash","duration_seconds":120}\n'
                '{"command":"listen","duration_seconds":30}\n', encoding="utf-8")
            clock = [1000.0]
            store = device_lock.LockStore(root / "locks", now=lambda: clock[0],
                                          records_root=records)
            held = store.acquire("COM5", "Alice", "firmware", kind="flash")
            clock[0] += 20
            store.enqueue("COM5", "Bob", "watch", kind="listen")
            store.enqueue("COM5", "Cara", "firmware", kind="flash")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                device_lock.print_status(store.status("COM5"), now=clock[0])
            result = out.getvalue()
            self.assertIn("estimated free", result)
            self.assertIn("1. Bob for watch", result)
            self.assertIn("2. Cara for firmware", result)
            self.assertLess(result.index("Bob"), result.index("Cara"))
            store.release("COM5", held["token"])


if __name__ == "__main__":
    unittest.main()
