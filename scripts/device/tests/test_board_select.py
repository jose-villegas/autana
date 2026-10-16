import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

from test_lock_guard import Store, plugged, usb, BOARD_A, BOARD_B
import isolation  # noqa: F401
import device
import device_lock


class SelectionTests(Store):
    def test_run_children_and_take_token_keep_selected_board(self):
        child = self.root / "child.py"
        child.write_text("""
import contextlib
import sys
from unittest import mock
sys.path.insert(0, sys.argv[1])
import device
sys.path.insert(0, sys.argv[2])
import autana
board_a, board_b, verb = sys.argv[3:6]
def screenshot(args, store, board):
    with device.HeldLock(store, board, "child", "screenshot", 0):
        print("child board=" + device.find_board().serial)
    return 0
def write_image(built, store, board, held):
    print("child board=" + device.find_board().serial)
    return "build", mock.Mock()
flags, words = autana.pop_globals(sys.argv[6:])
with autana.applied(flags), \
     mock.patch.object(device, "plugged_boards", return_value=[device.Board(board_a, "COM5"), device.Board(board_b, "COM7")]), \
     mock.patch.object(device, "screenshot", screenshot), \
     mock.patch.object(device, "build_image", return_value=contextlib.nullcontext(None)), \
     mock.patch.object(device, "write_image", write_image), \
     mock.patch.object(device, "await_console"):
    raise SystemExit(device.main([verb, *words]))
""", encoding="utf-8")
        command = [sys.executable, str(child), str(Path(device.__file__).parent),
                   str(Path(device.__file__).parents[1] / "autana"), BOARD_A, BOARD_B]
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")), \
             mock.patch.object(device_lock, "default_root", return_value=self.store.root), \
             mock.patch.dict(os.environ, {device.BOARD_ENV: "", device.TOKEN_ENV: "",
                                         "_AUTANA_DEVICE_LOCK_ROOT": str(self.store.root)}):
            for verb, words in (("screenshot", []),
                                ("flash", ["--variant", "dev", "--worktree", "."])):
                with self.subTest(verb=verb), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(device.main(["run", "--", *command, verb, *words]), 0)
            with contextlib.redirect_stdout(io.StringIO()) as output, \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(device.main(["take", "--for", "10", "job"]), 0)
            token = output.getvalue().strip()
            try:
                result = subprocess.run([*command, "flash", "--lock-token", token,
                                         "--variant", "dev", "--worktree", "."],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("child board=" + BOARD_A, result.stdout)
            finally:
                self.store.release(BOARD_A, token)

    def test_explicit_tokens_resolve_auto_picked_hold_with_multiple_boards(self):
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")), \
             mock.patch.object(device_lock, "default_root", return_value=self.store.root), \
             mock.patch.dict(os.environ, {device.BOARD_ENV: "", device.TOKEN_ENV: ""}), \
             contextlib.redirect_stderr(io.StringIO()):
            for command in ("check-token", "release"):
                with self.subTest(command=command), contextlib.redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(device.main(["take", "--for", "10", "job"]), 0)
                    token = output.getvalue().strip()
                    try:
                        self.assertEqual(device.main([command, "--token", token]), 0)
                        if command == "release":
                            self.assertIsNone(self.store.status(BOARD_A)["lock"])
                    finally:
                        self.store.release(BOARD_A, token)

    def test_auto_pick_accounts_for_every_queued_duration(self):
        duration = 10
        for board in (BOARD_A, BOARD_B):
            held = self.store.acquire(board, "holder", "flash")
            self.store.hold(board, held["token"], "job", seconds=60,
                            estimate=duration if board == BOARD_A else 2 * duration)
        for index in range(2):
            self.store.enqueue(BOARD_A, f"waiter-{index}", "flash")
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")), \
             mock.patch.object(device_lock, "duration_history", return_value={"flash": duration}):
            entries = device.board_statuses(self.store)
        self.assertLess(entries[0]["estimated_free"], entries[1]["estimated_free"])
        self.assertGreater(entries[0]["waiting"][-1]["estimated_start"] + duration,
                           entries[1]["estimated_free"])
        self.assertEqual(device.pick_board(entries)[0], BOARD_B)

    def test_pick_empty_and_all_human_reserved(self):
        with self.assertRaises(device.NoBoard):
            device.pick_board([])
        for board in (BOARD_A, BOARD_B):
            self.store.reserve_human(board, "maintainer", "bench")
        self.store.enqueue(BOARD_A, "waiter", "flash")
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")):
            self.assertEqual(device.pick_board(device.board_statuses(self.store)),
                             (BOARD_B, "shortest queue"))

    def test_queued_board_is_not_displayed_as_free(self):
        self.store.enqueue(BOARD_A, "waiter", "flash")
        with plugged(usb(BOARD_A, "COM5")):
            entries = device.board_statuses(self.store)
        for printer, args in ((device.print_devices, (entries,)),
                              (device.print_statuses, (entries, self.store.now()))):
            with contextlib.redirect_stdout(io.StringIO()) as output:
                printer(*args)
            self.assertIn("A308 " + BOARD_A + " COM5", output.getvalue())
            self.assertIn("queued", output.getvalue())
            self.assertNotIn(": free", output.getvalue())

    def test_short_ids_resolve_usb_seen_and_case(self):
        self.store.note_seen(BOARD_B)
        with plugged(usb(BOARD_A, "COM5")):
            self.assertEqual(device.resolve_board("a308", self.store), BOARD_A)
            self.assertEqual(device.resolve_board("b122", self.store), BOARD_B)
            self.assertEqual(device.resolve_board(BOARD_A.lower(), self.store), BOARD_A)

    def test_short_id_errors_name_known_and_ambiguous_boards(self):
        collision = "00:00:00:00:A3:08"
        self.store.note_seen(collision)
        with plugged(usb(BOARD_A, "COM5")):
            for value, expected in (("FFFF", "known boards"), ("a308", "full serial")):
                with self.subTest(value=value), self.assertRaisesRegex(RuntimeError, expected) as caught:
                    device.resolve_board(value, self.store)
                self.assertIn(BOARD_A, str(caught.exception))
                self.assertIn(collision, str(caught.exception))

    def test_devices_text_and_json_share_status_data(self):
        entries = [dict(board=BOARD_A, port="COM5", state="unlocked", holder=None,
                        since=None, estimated_free=None, waiting=[], expires_at=None, lapsed=None),
                   dict(board=BOARD_B, port=None, state="human", holder=dict(owner="person"),
                        since=0, estimated_free=None, waiting=[], expires_at=3600, lapsed=None)]
        with mock.patch.object(device, "board_statuses", return_value=entries), plugged():
            for verb in ("devices", "status"):
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(device.main([verb, "--json"]), 0)
                self.assertEqual(json.loads(output.getvalue()), dict(boards=entries))
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(device.main(["devices"]), 0)
            lines = output.getvalue().splitlines()
            self.assertEqual(len(lines), 2)
            for word in ("A308", BOARD_A, "COM5", "free"):
                self.assertIn(word, lines[0])
            for word in ("B122", BOARD_B, "not on USB", "reserved by a human", "person"):
                self.assertIn(word, lines[1])

    def test_auto_pick_prefers_free_then_estimate_then_queue(self):
        def entry(board, state="held", end=None, wait=0):
            return dict(board=board, port="COM5", state=state, estimated_free=end, estimated_start=end,
                        waiting=[{}] * wait)
        cases = [([entry(BOARD_A, end=1), entry(BOARD_B, "unlocked")], BOARD_B),
                 ([entry(BOARD_A, end=20), entry(BOARD_B, end=10)], BOARD_B),
                 ([entry(BOARD_A, wait=2), entry(BOARD_B, wait=1)], BOARD_B),
                 ([entry(BOARD_A, "unlocked", wait=2), entry(BOARD_B, "unlocked")], BOARD_B),
                 ([entry(BOARD_A, "human"), entry(BOARD_B)], BOARD_B)]
        for entries, expected in cases:
            with self.subTest(entries=entries):
                self.assertEqual(device.pick_board(entries)[0], expected)

    def test_auto_pick_is_opt_in_and_only_considers_usb(self):
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")):
            self.store.acquire(BOARD_A, "holder", "flash")
            with self.assertRaisesRegex(RuntimeError, "--board.*-b.*use"):
                device.board_for_lock(self.store)
            with contextlib.redirect_stderr(io.StringIO()) as output:
                self.assertEqual(device.board_for_lock(self.store, auto_pick=True), (BOARD_B, "free, nobody queued"))
            self.assertIn(BOARD_B, output.getvalue())
            self.assertIn("free", output.getvalue())

    def test_command_declarations_control_auto_pick(self):
        cases = [(["flash", "--variant", "dev", "--worktree", "."], True),
                 (["selftest", "--worktree", "."], True),
                 (["batch", "--worktree", ".", "--suite", "x"], True),
                 (["batch", "--worktree", ".", "--suite", "x", "--no-flash"], False),
                 (["run", "--", "echo", "x"], True),
                 (["take", "--for", "10", "job"], True),
                 (["listen", "--seconds", "1"], False),
                 (["screenshot"], False), (["reset"], False),
                 (["run-suite", "x"], False)]
        for args, expected in cases:
            with self.subTest(args=args), \
                 mock.patch.object(device, "board_for_lock", side_effect=RuntimeError("stop")) as choose, \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(device.main(args), 1)
                self.assertEqual(choose.call_args.kwargs["auto_pick"], expected)

    def test_auto_picked_final_verdict_names_board(self):
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")), \
             mock.patch.object(device_lock, "default_root", return_value=self.store.root), \
             mock.patch.object(device, "reset_device", return_value=0), \
             mock.patch.object(device, "build_image", side_effect=RuntimeError("build failed")), \
             contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(device.main(["flash", "--variant", "dev", "--worktree", "."]), 1)
        self.assertIn("FAIL (1): board A308 (" + BOARD_A + ")", output.getvalue().splitlines()[-1])


if __name__ == "__main__":
    unittest.main()
