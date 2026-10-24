"""perf_rows: one line per board from parsed captures, never pooled across boards."""
import contextlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import perf_rows

A308 = "90:70:69:FE:A3:08"
B2124 = "44:BD:8D:78:21:24"
ROW = "full-screen gas cascade"
TODAY = datetime(2026, 10, 24, 12, 0)


def target(name, measured, goal):
    return f"I (1) device_tests: PERF TARGET {name}: measured {measured} us, goal {goal} us, distance +0%\n"


class Records:
    """A records folder: index.jsonl plus one capture file per entry."""

    def __init__(self, root):
        self.root = pathlib.Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.entries = []

    def flash(self, board, build, flags):
        self.entries.append(dict(command="flash", board=board, build_id=build, image_flags=flags,
                                 started_at="2026-10-01T00:00:00", reason=None))

    def capture(self, board, build, when, text, reason="complete", suite="run_sand_perf_suite"):
        path = self.root / f"{len(self.entries)}.log"
        path.write_text(text, encoding="utf-8")
        self.entries.append(dict(command="run-suite", board=board, build_id=build, started_at=when,
                                 reason=reason, suite=suite, capture_path=str(path),
                                 owner="someone@HOSTNAME:1", worktree="/srv/private-worktree",
                                 commit="0123456789abcdef0123"))
        (self.root / "index.jsonl").write_text("".join(json.dumps(e) + "\n" for e in self.entries),
                                               encoding="utf-8")
        return path


class Isolated(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.temp.name)
        patches = [patch.object(perf_rows, "parsed_cache", return_value=self.dir / "cache/parsed.jsonl"),
                   patch.object(perf_rows.query_log, "log_dir", return_value=self.dir / "usage")]
        for item in patches:
            item.start()
            self.addCleanup(item.stop)
        self.addCleanup(self.temp.cleanup)
        self.records = Records(self.dir / "records")

    def ask(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), patch.object(perf_rows, "datetime") as clock:
            clock.now.return_value = TODAY
            clock.fromisoformat = datetime.fromisoformat
            status = perf_rows.main([*argv, "--store", str(self.dir / "nostore"),
                                     "--records", str(self.records.root)])
        return status, out.getvalue()


class CaptureRows(unittest.TestCase):
    def test_pegged_unpegged_and_mean_rows(self):
        text = (target(ROW, 177000, 179400) + target(ROW, 177200, 179400)
                + "I (2) device_tests: PERF TARGET new row: measured 900 us, goal not yet pegged\n"
                + "I (3) skin_light_perf: fused_table_1 one core: mean 1903us\n")
        self.assertEqual(perf_rows.capture_rows(text), {
            ROW: [179400, 177000, 177200], "new row": [None, 900], "fused_table_1": [None, 1903]})

    def test_line_carries_no_path_or_user(self):
        entry = dict(board=A308, build_id="b-diag", started_at="2026-10-24T10:00:00.123", suite="s",
                     commit="0123456789abcdef", owner="someone@HOSTNAME:1", worktree="/srv/private-worktree")
        line = perf_rows.capture_line(entry, {"b-diag": (True, 7)}, target(ROW, 1, 2))
        self.assertEqual(line, {"board": "A308", "build": "b-diag", "commit": "0123456789ab", "suite": "s",
                                "date": "2026-10-24T10:00:00", "scope": True, "seed": 7,
                                "rows": {ROW: [2, 1]}})


class Lookup(Isolated):
    def test_one_line_per_board_never_pooled(self):
        r = self.records
        r.capture(A308, "a1-diag", "2026-10-22T10:00:00", target(ROW, 177000, 179400))
        r.capture(A308, "a1-diag", "2026-10-22T10:05:00", target(ROW, 177300, 179400))
        r.capture(B2124, "b1-diag", "2026-10-23T10:00:00", target(ROW, 181000, 179400))
        status, out = self.ask(ROW)
        self.assertEqual(status, 0)
        lines = out.splitlines()
        self.assertEqual(len(lines), 3)
        # Two runs of one build pool on their own board; the other board's sample never enters.
        self.assertTrue(lines[1].startswith("2124 "))
        self.assertIn("worst 181000", lines[1])
        self.assertIn("OVER GOAL", lines[1])
        self.assertTrue(lines[2].startswith("A308 "))
        self.assertIn("worst 177300", lines[2])
        self.assertIn("+1.2%", lines[2])
        self.assertIn("noise 0.2%", lines[2])
        self.assertNotIn("181000", lines[2])

    def test_flags_moved_repegged_stale_unpegged(self):
        r = self.records
        r.capture(A308, "a1-diag", "2026-10-10T10:00:00", target(ROW, 170000, 176900))
        r.capture(A308, "a2-diag", "2026-10-11T10:00:00", target(ROW, 177000, 179400)
                  + "I (2) device_tests: PERF TARGET fresh: measured 5 us, goal not yet pegged\n")
        _, out = self.ask(ROW)
        self.assertIn("trend +4.1% vs 1 before", out)
        self.assertIn("moved +4.1%", out)
        self.assertIn("re-pegged", out)
        self.assertIn("stale 13d", out)
        _, out = self.ask("fresh")
        self.assertIn("goal -", out)
        self.assertIn("unpegged", out)

    def test_many_rows_list_names_and_counts_only(self):
        self.records.capture(A308, "a1-diag", "2026-10-24T10:00:00",
                             target(ROW, 1, 2) + target(ROW + ", one core", 3, 4))
        status, out = self.ask("gas cascade")
        self.assertEqual(status, 1)
        self.assertEqual(out.splitlines(), ["2 rows match; name one (captures in brackets):",
                                            f"  {ROW} (1)", f"  {ROW}, one core (1)"])
        _, out = self.ask(ROW)
        self.assertIn("worst 1", out)

    def test_board_drill_down_and_runs(self):
        r = self.records
        r.capture(A308, "a1-diag", "2026-10-23T10:00:00", target(ROW, 100, 200))
        r.capture(B2124, "b1-diag", "2026-10-23T10:00:00", target(ROW, 300, 200))
        _, out = self.ask(ROW, "-b", "a308", "--runs")
        self.assertNotIn("2124", out)
        self.assertIn("a1-diag  2026-10-23 10:00  runs 1  worst 100  median 100  +50.0%  goal 200  ?", out)
        self.assertIn("    100", out)

    def test_scoped_images_hidden_unless_asked(self):
        r = self.records
        r.flash(A308, "s-diag", ["--perf-scope", "--layout-seed", "9"])
        r.flash(A308, "f-diag", [])
        r.capture(A308, "s-diag", "2026-10-23T10:00:00", target(ROW, 111, 200))
        r.capture(A308, "f-diag", "2026-10-23T11:00:00", target(ROW, 222, 200))
        _, out = self.ask(ROW)
        self.assertIn("worst 222", out)
        self.assertNotIn("111", out)
        _, out = self.ask(ROW, "--scoped", "-b", "A308")
        self.assertIn("scoped seed 9", out)
        self.assertNotIn("222", out)

    def test_incomplete_captures_are_skipped_and_parsed_once(self):
        r = self.records
        r.capture(A308, "x-diag", "2026-10-23T09:00:00", target(ROW, 999, 200), reason="timeout")
        path = r.capture(A308, "a1-diag", "2026-10-23T10:00:00", target(ROW, 100, 200))
        _, out = self.ask(ROW)
        self.assertNotIn("999", out)
        path.unlink()
        _, out = self.ask(ROW)
        self.assertIn("worst 100", out)

    def test_budget_caps_output(self):
        for index in range(6):
            self.records.capture(A308, f"a{index}-diag", f"2026-10-2{index}T10:00:00", target(ROW, 100, 200))
        _, out = self.ask(ROW, "-b", "A308", "--budget", "60")
        self.assertLessEqual(len(out), 60 * perf_rows.query_log.CHARS_PER_TOKEN + 40)
        self.assertIn("more lines past --budget", out)

    def test_zero_matches_offer_nearest_words(self):
        self.records.capture(A308, "a1-diag", "2026-10-23T10:00:00", target(ROW, 100, 200))
        status, out = self.ask("cascdae")
        self.assertEqual(status, 1)
        self.assertIn("nearest words: cascade", out)


def git(folder, *args):
    subprocess.run(["git", "-C", str(folder), *args], check=True, capture_output=True, stdin=subprocess.DEVNULL)


class Sync(Isolated):
    def sync(self, store, host):
        with contextlib.redirect_stdout(io.StringIO()):
            return perf_rows.main(["sync", "--host", host, "--store", str(store),
                                   "--records", str(self.records.root)])

    def test_two_hosts_share_one_board_without_conflict(self):
        remote = self.dir / "remote.git"
        subprocess.run(["git", "init", "-q", "--bare", str(remote)], check=True, stdin=subprocess.DEVNULL)
        stores = []
        for name in ("legion", "helios"):
            store = self.dir / name
            store.mkdir()
            git(store, "init", "-q")
            git(store, "remote", "add", "origin", str(remote))
            git(store, "checkout", "-q", "--orphan", perf_rows.BRANCH)
            stores.append(store)
        self.records.capture(A308, "a1-diag", "2026-10-23T10:00:00", target(ROW, 100, 200))
        self.sync(stores[0], "Legion")
        # The board moved: the second machine records a capture of the same board that day.
        with patch.object(perf_rows, "parsed_cache", return_value=self.dir / "cache2.jsonl"):
            self.records = Records(self.dir / "records2")
            self.records.capture(A308, "a2-diag", "2026-10-23T15:00:00", target(ROW, 120, 200))
            self.sync(stores[1], "Helios")
        files = sorted(p.relative_to(stores[1]).as_posix()
                       for p in (stores[1] / "captures").rglob("*.jsonl"))
        self.assertEqual(files, ["captures/A308/2026-10/2026-10-23-Helios.jsonl",
                                 "captures/A308/2026-10/2026-10-23-Legion.jsonl"])
        text = "".join(p.read_text() for p in (stores[1] / "captures").rglob("*.jsonl"))
        for private in ("someone", "HOSTNAME", "private-worktree", ".log"):
            self.assertNotIn(private, text)
        self.assertEqual(self.sync(stores[1], "Helios"), 0)
        self.assertEqual(sum(1 for _ in text.splitlines()), 2)

    def test_host_must_be_a_short_name(self):
        with self.assertRaises(SystemExit):
            self.sync(self.dir / "store", "JV.local/user")


class Eval(unittest.TestCase):
    def test_fixture_questions_all_answer_first(self):
        with tempfile.TemporaryDirectory() as folder, \
                patch.object(perf_rows.query_log, "log_dir", return_value=pathlib.Path(folder)), \
                contextlib.redirect_stdout(io.StringIO()):
            perf_rows.main(["--eval"])
            result = json.loads((pathlib.Path(folder) / perf_rows.query_log.EVAL_HISTORY).read_text())
        self.assertEqual(result["top1"], 1.0)


if __name__ == "__main__":
    unittest.main()
