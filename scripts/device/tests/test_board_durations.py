"""Board-specific duration selection and retention."""
import isolation  # noqa: F401
import json
import tempfile
import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import device_lock as lock


class BoardDurations(unittest.TestCase):
    def test_own_history_and_fallback(self):
        with tempfile.TemporaryDirectory() as root:
            for board, seconds in (("A", 10), ("B", 100)):
                for _ in range(lock.ESTIMATE_MINIMUM_RUNS):
                    lock.record_duration("flash", seconds, root=root, board=board)
            self.assertEqual(lock.duration_history(root, board="A")["flash"], 10)
            self.assertEqual(lock.duration_history(root, board="B")["flash"], 100)
            self.assertEqual(lock.duration_history(root, board="C")["flash"], 55)
            rows = lock.duration_rows(root)
            self.assertEqual({r["board"] for r in rows}, {"A", "B"})

    def test_legacy_rows_and_failed_runs(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / lock.DURATIONS_FILE
            path.write_text(''.join(json.dumps(dict(command="flash", duration_seconds=30)) + "\n"
                                    for _ in range(lock.ESTIMATE_MINIMUM_RUNS)))
            for _ in range(lock.ESTIMATE_MINIMUM_RUNS - 1):
                lock.record_duration("flash", 10, root=root, board="A")
            lock.record_duration("flash", 999, error="failed", root=root, board="A")
            self.assertEqual(lock.duration_history(root, board="A")["flash"], 30)
            lock.record_duration("flash", 10, root=root, board="A")
            self.assertEqual(lock.duration_history(root, board="A")["flash"], 10)

    def test_trim_keeps_each_board(self):
        with tempfile.TemporaryDirectory() as root:
            rows = [dict(command="flash", duration_seconds=n, board=board)
                    for board in ("A", "B", None)
                    for n in range(lock.ESTIMATE_RECENT_RUNS + 1)]
            path = Path(root) / lock.DURATIONS_FILE
            lock.trim_durations(path, rows)
            kept = lock.duration_rows(root)
            self.assertEqual(len(kept), 3 * lock.ESTIMATE_RECENT_RUNS)
            self.assertTrue(all(r["duration_seconds"] > 0 for r in kept))

    def test_status_and_auto_pick_use_board_history(self):
        import device
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as root:
            store = lock.LockStore(root, now=lambda: 100, is_alive=lambda pid: True)
            boards = ("90:70:69:FE:A3:08", "90:70:69:FE:B4:09")
            for board, duration in zip(boards, (100, 10)):
                for _ in range(lock.ESTIMATE_MINIMUM_RUNS):
                    lock.record_duration("flash", duration, root=root, board=board)
                ticket = store.enqueue(board, "test", "flash")
                store.claim(board, ticket)
            with patch.object(device, "plugged_boards", return_value=[]):
                entries = device.board_statuses(store)
            self.assertEqual([entry["estimated_free"] for entry in entries], [200, 110])
            self.assertEqual(device.pick_board(entries)[0], boards[1])
