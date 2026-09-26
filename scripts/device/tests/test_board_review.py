import isolation  # noqa: F401  (first: keeps the suite out of real records)
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE))
import device
import device_lock
sys.path.insert(0, str(DEVICE.parent / "autana"))
import autana


class GuardTests(unittest.TestCase):
    @mock.patch.object(device, "find_port", return_value="COM5")
    def test_guard_rejects_missing_replaced_and_stale_lock(self, _find_port):
        with tempfile.TemporaryDirectory() as directory:
            store = device_lock.LockStore(directory, now=lambda: 1000)
            with self.assertRaisesRegex(RuntimeError, "device lock"):
                device.require_port_lock("COM5")
            with device.HeldLock(store, "COM5", "agent", "send", 0) as held:
                device.require_port_lock("COM5")
                path = store.lock_path("COM5")
                saved = path.read_bytes()
                changed = json.loads(saved)
                changed["token"] = "replacement"
                store.write_json(path, changed)
                with self.assertRaisesRegex(RuntimeError, "device lock was lost"):
                    device.require_port_lock("COM5")
                changed["token"] = held.held["token"]
                changed["heartbeat_at"] = 1
                store.write_json(path, changed)
                with self.assertRaisesRegex(RuntimeError, "device lock was lost"):
                    device.require_port_lock("COM5")
                changed["heartbeat_at"] = 1000
                store.write_json(path, changed)

    def test_nested_lock_restores_outer(self):
        with tempfile.TemporaryDirectory() as directory:
            outer = device_lock.LockStore(Path(directory) / "outer")
            inner = device_lock.LockStore(Path(directory) / "inner")
            with mock.patch.object(device, "find_port", return_value="COM5"):
                with device.HeldLock(outer, "COM5", "a", "send", 0) as held_outer:
                    with device.HeldLock(inner, "COM5", "b", "send", 0) as held_inner:
                        self.assertIs(device.ACTIVE_LOCK.held, held_inner)
                    self.assertIs(device.ACTIVE_LOCK.held, held_outer)
                    device.require_port_lock("COM5")

    def test_port_must_belong_to_the_locked_usb_board(self):
        with tempfile.TemporaryDirectory() as directory:
            store = device_lock.LockStore(directory, board_id=device.BOARD_ID)
            with mock.patch.object(device, "find_port", return_value="COM7"):
                with device.HeldLock(store, "COM5", "agent", "send", 0):
                    device.require_port_lock("COM7")
                    with self.assertRaisesRegex(RuntimeError, "port does not belong"):
                        device.require_port_lock("COM5")

    def test_replaced_lock_stops_capture_during_read(self):
        with tempfile.TemporaryDirectory() as directory:
            store = device_lock.LockStore(Path(directory) / "locks")
            class Replacer:
                def read(self, unused_size):
                    lock = store.read_json(store.lock_path("COM5"))
                    lock["token"] = "replacement"
                    store.write_json(store.lock_path("COM5"), lock)
                    return b"some output\n"
            with device.HeldLock(store, "COM5", "first", "listen", 0) as held:
                with self.assertRaisesRegex(RuntimeError, "device lock was lost"):
                    device.capture(Replacer(), Path(directory) / "capture.log", 1, None)
                store.write_json(store.lock_path("COM5"), held.held)

    def test_lost_lock_during_usb_wait_fails_with_lock_message(self):
        with tempfile.TemporaryDirectory() as directory:
            store = device_lock.LockStore(directory)
            with device.HeldLock(store, "COM5", "agent", "reset", 0) as held:
                def reclaim(unused_seconds):
                    store.write_json(store.lock_path("COM5"),
                                     dict(held.held, token="replacement"))
                with self.assertRaisesRegex(RuntimeError, "device lock was lost"):
                    device.open_when_free("COM5", seconds=1,
                                          opener=mock.Mock(side_effect=OSError("USB missing")),
                                          sleep=reclaim)
                store.write_json(store.lock_path("COM5"), held.held)

    def test_second_process_cannot_pass_guard_and_reclaim_stops_first(self):
        with tempfile.TemporaryDirectory() as directory:
            store = device_lock.LockStore(directory)
            probe = ("import sys; from pathlib import Path; "
                     "sys.path.insert(0, sys.argv[1]); import device_lock; "
                     "s=device_lock.LockStore(sys.argv[2]); "
                     "print(s.acquire('COM5', 'second', 'send', wait=0)); "
                     "print(s.check_token('COM5', sys.argv[3]))")
            with device.HeldLock(store, "COM5", "first", "listen", 0) as held:
                result = subprocess.run([sys.executable, "-c", probe, str(DEVICE),
                                         directory, "foreign"], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout.strip(), "None\nFalse")
                reclaim = ("import sys,time; sys.path.insert(0, sys.argv[1]); "
                           "import device_lock; s=device_lock.LockStore(sys.argv[2], "
                           "now=lambda: time.time()+601); "
                           "print(bool(s.acquire('COM5', 'second', 'send', wait=0)))")
                result = subprocess.run([sys.executable, "-c", reclaim, str(DEVICE), directory],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout.strip(), "True")
                with self.assertRaisesRegex(RuntimeError, "device lock was lost"):
                    device.require_port_lock("COM5")
                store.write_json(store.lock_path("COM5"), held.held)

    def test_cli_check_token_rejects_foreign_missing_stale_and_dead(self):
        with tempfile.TemporaryDirectory() as directory:
            store = device_lock.LockStore(directory)
            held = store.acquire("COM5", "agent", "flash")
            def check(token):
                return subprocess.run([sys.executable, str(DEVICE / "device_lock.py"),
                                       "--root", directory, "--port", "COM5", "check-token",
                                       "--token", token], capture_output=True).returncode
            self.assertEqual(check(held["token"]), 0)
            self.assertNotEqual(check("foreign"), 0)
            store.lock_path("COM5").unlink()
            self.assertNotEqual(check(held["token"]), 0)
            stale = dict(held, heartbeat_at=1)
            store.write_json(store.lock_path("COM5"), stale)
            self.assertNotEqual(check(held["token"]), 0)
            dead = dict(held, heartbeat_at=device_lock.time.time(), pid=99999999)
            store.write_json(store.lock_path("COM5"), dead)
            self.assertNotEqual(check(held["token"]), 0)

    def test_flash_script_check_only_refuses_without_live_token(self):
        with tempfile.TemporaryDirectory() as directory:
            store = device_lock.LockStore(directory, board_id=device.BOARD_ID)
            held = store.acquire("COM5", "agent", "flash")
            script = DEVICE.parents[1] / "launcher" / "tools" / "build" / "build_flash.sh"
            env = dict(os.environ, AUTANA_DEVICE_LOCK_ROOT=directory,
                       AUTANA_DEVICE_LOCK_TOKEN=held["token"])
            command = [device.git_bash(), str(script), "--check-flash-lock", "COM5"]
            self.assertEqual(subprocess.run(command, env=env, capture_output=True).returncode, 0)
            env["AUTANA_DEVICE_LOCK_TOKEN"] = "foreign"
            refused = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("device lock token is not active", refused.stderr)

    def test_status_reads_the_lock_while_the_board_is_off_usb(self):
        with tempfile.TemporaryDirectory() as directory:
            device_lock.LockStore(directory, now=time.time).acquire("COM5", "a", "flash", wait=0)
            output = io.StringIO()
            with mock.patch.object(device, "find_port",
                                   side_effect=RuntimeError("no USB Serial/JTAG board found")), \
                    mock.patch.object(device_lock, "default_root", return_value=Path(directory)), \
                    mock.patch.object(sys, "argv", ["device.py", "status"]), \
                    contextlib.redirect_stdout(output):
                self.assertEqual(device.main(), 0)
            self.assertIn("held by a for flash", output.getvalue())

    def test_every_default_store_shares_one_lock_across_com_names(self):
        with tempfile.TemporaryDirectory() as directory:
            first = device_lock.LockStore(directory, now=lambda: 1000)
            second = device_lock.LockStore(directory, now=lambda: 1000)
            self.assertTrue(first.acquire("COM5", "a", "flash", wait=0))
            self.assertFalse(second.acquire("COM7", "b", "listen", wait=0))


class EstimateTests(unittest.TestCase):
    def test_history_excludes_errors_and_bad_values_and_uses_last_thirty(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = ([{"command": "flash", "duration_seconds": i} for i in range(35)] +
                    [{"command": "flash", "duration_seconds": 999, "error": "failed"},
                     {"command": "flash", "duration_seconds": -1},
                     {"command": "flash", "duration_seconds": "bad"},
                     {"command": "flash"}])
            (Path(directory) / "durations.jsonl").write_text(
                "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
            self.assertEqual(device_lock.duration_history(directory)["flash"], 19.5)

    def test_three_successful_runs_are_needed_for_estimate(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "durations.jsonl"
            path.write_text('{"command":"send","duration_seconds":2}\n'
                            '{"command":"send","duration_seconds":4}\n', encoding="utf-8")
            self.assertEqual(device_lock.duration_history(directory), {})
            with path.open("a", encoding="utf-8") as stream:
                stream.write('{"command":"send","duration_seconds":6}\n')
            self.assertEqual(device_lock.duration_history(directory), {"send": 4})

    def test_each_held_command_records_its_own_duration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            store = device_lock.LockStore(root / "locks")
            with mock.patch.object(device, "records_root", return_value=root):
                with device.HeldLock(store, "COM5", "agent", "batch", 0) as outer:
                    time.sleep(0.02)
                    args = mock.Mock()
                    with device.holding(store, "COM5", args, outer, "flash"):
                        time.sleep(0.02)
                    time.sleep(0.02)
            rows = [json.loads(line) for line in (root / "durations.jsonl").read_text().splitlines()]
            by_kind = {row["command"]: row["duration_seconds"] for row in rows}
            self.assertGreater(by_kind["batch"], by_kind["flash"])
            self.assertEqual(set(by_kind), {"batch", "flash"})

    def test_holder_and_waiter_estimates_chain_and_stop_at_unknown(self):
        status = {"human": None, "reclaimable": None,
                  "lock": {"owner": "A", "purpose": "flash", "kind": "flash",
                           "acquired_at": 1000},
                  "queue": [{"ticket": "b", "owner": "B", "purpose": "send", "kind": "send"},
                            {"ticket": "c", "owner": "C", "purpose": "unknown", "kind": "x"},
                            {"ticket": "d", "owner": "D", "purpose": "send", "kind": "send"}],
                  "durations": {"flash": 100, "send": 20}}
        self.assertEqual(device_lock.queue_estimates(status, 1050, status["durations"]),
                         {"b": 1100, "c": 1120, "d": None})
        self.assertEqual(device_lock.queue_estimates(status, 1200, status["durations"])["b"], 1200)
        status["human"] = {"owner": "person", "note": "bench", "since_at": 1000}
        self.assertTrue(all(value is None for value in
                            device_lock.queue_estimates(status, 1050, status["durations"]).values()))

    def test_json_parser_reads_the_text_status_variants(self):
        status = {"human": None, "reclaimable": None,
                  "lock": {"owner": "agent", "purpose": "flash", "kind": "flash",
                           "acquired_at": 1000},
                  "queue": [{"ticket": "one", "owner": "waiter", "purpose": "send",
                             "kind": "send"}]}
        def render(durations):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                device_lock.print_status(status, now=1050, durations=durations)
            return autana.parse_status(output.getvalue())
        unknown = render({})
        self.assertEqual(unknown["elapsed_seconds"], 50)
        self.assertIsNone(unknown["estimated_free"])
        self.assertIsNone(unknown["waiting"][0]["estimated_start"])
        known = render({"flash": 100, "send": 20})
        self.assertEqual(known["estimated_free"], device_lock.local_time(1100))
        self.assertEqual(known["waiting"][0]["estimated_start"],
                         device_lock.local_time(1100))
        status["human"] = {"owner": "person", "note": "bench", "since_at": 1000}
        human = render({"flash": 100, "send": 20})
        self.assertEqual(human["state"], "human")
        self.assertIsNone(human["waiting"][0]["estimated_start"])


if __name__ == "__main__":
    unittest.main()
