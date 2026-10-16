import contextlib
import io
import json
import unittest
from unittest import mock

from test_lock_guard import Store, plugged, usb, BOARD_A, BOARD_B
import device
import device_lock


class SelectionTests(Store):
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
            return dict(board=board, port="COM5", state=state, estimated_free=end,
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
                self.assertEqual(device.board_for_lock(self.store, auto_pick=True), BOARD_B)
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
