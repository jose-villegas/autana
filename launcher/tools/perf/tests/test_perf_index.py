"""perf_index: captures parsed into per-board, per-day, per-machine store files; no path or user leaves."""
import contextlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import perf_index

A308 = "90:70:69:FE:A3:08"
ROW = "full-screen gas cascade"


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
        cache = patch.object(perf_index, "parsed_cache", return_value=self.dir / "cache/parsed.jsonl")
        cache.start()
        self.addCleanup(cache.stop)
        self.addCleanup(self.temp.cleanup)
        self.records = Records(self.dir / "records")

    def lines(self, *folders):
        return perf_index.local_lines([pathlib.Path(f) / "index.jsonl" for f in folders or [self.records.root]])


class CaptureRows(unittest.TestCase):
    def test_pegged_unpegged_and_mean_rows(self):
        text = (target(ROW, 177000, 179400) + target(ROW, 177200, 179400)
                + "I (2) device_tests: PERF TARGET new row: measured 900 us, goal not yet pegged\n"
                + "I (3) skin_light_perf: fused_table_1 one core: mean 1903us\n")
        self.assertEqual(perf_index.capture_rows(text), {
            ROW: [179400, 177000, 177200], "new row": [None, 900], "fused_table_1": [None, 1903]})

    def test_line_carries_no_path_or_user(self):
        entry = dict(board=A308, build_id="b-diag", started_at="2026-10-24T10:00:00.123", suite="s",
                     commit="0123456789abcdef", owner="someone@HOSTNAME:1", worktree="/srv/private-worktree")
        line = perf_index.capture_line(entry, target(ROW, 1, 2))
        self.assertEqual(line, {"board": "A308", "build": "b-diag", "commit": "0123456789ab", "suite": "s",
                                "date": "2026-10-24T10:00:00", "rows": {ROW: [2, 1]}})


class LocalLines(Isolated):
    def test_incomplete_captures_are_skipped_and_each_parsed_once(self):
        r = self.records
        r.capture(A308, "x-diag", "2026-10-23T09:00:00", target(ROW, 999, 200), reason="timeout")
        path = r.capture(A308, "a1-diag", "2026-10-23T10:00:00", target(ROW, 100, 200))
        self.assertEqual([line["build"] for line in self.lines()], ["a1-diag"])
        path.unlink()
        self.assertEqual([line["rows"][ROW] for line in self.lines()], [[200, 100]])

    def test_only_the_named_records_answer(self):
        self.records.capture(A308, "a1-diag", "2026-10-23T10:00:00", target(ROW, 100, 200))
        self.lines()
        other = Records(self.dir / "other")
        other.capture(A308, "a2-diag", "2026-10-23T11:00:00", target(ROW, 150, 200))
        self.assertEqual([line["build"] for line in self.lines(other.root)], ["a2-diag"])

    def test_flags_join_from_every_records_folder(self):
        flashes = Records(self.dir / "flashes")
        flashes.flash(A308, "t-diag", ["--hot-tunables"])
        (flashes.root / "index.jsonl").write_text("".join(json.dumps(e) + "\n" for e in flashes.entries))
        self.records.capture(A308, "t-diag", "2026-10-23T10:00:00", target(ROW, 111, 200))
        self.records.capture(A308, "u-diag", "2026-10-23T11:00:00", target(ROW, 112, 200))
        images = {line["build"]: line["image"] for line in self.lines(self.records.root, flashes.root)}
        self.assertEqual(images, {"t-diag": ["--hot-tunables"], "u-diag": None})


def git(folder, *args):
    subprocess.run(["git", "-C", str(folder), *args], check=True, capture_output=True, stdin=subprocess.DEVNULL)


class Sync(Isolated):
    def sync(self, store, host):
        with contextlib.redirect_stdout(io.StringIO()):
            return perf_index.main(["sync", "--host", host, "--store", str(store),
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
            git(store, "checkout", "-q", "--orphan", perf_index.BRANCH)
            stores.append(store)
        self.records.capture(A308, "a1-diag", "2026-10-23T10:00:00", target(ROW, 100, 200))
        self.sync(stores[0], "hostA")
        # The board moved: the second machine records a capture of the same board that day.
        with patch.object(perf_index, "parsed_cache", return_value=self.dir / "cache2.jsonl"):
            self.records = Records(self.dir / "records2")
            self.records.capture(A308, "a2-diag", "2026-10-23T15:00:00", target(ROW, 120, 200))
            self.sync(stores[1], "hostB")
        files = sorted(p.relative_to(stores[1]).as_posix()
                       for p in (stores[1] / "captures").rglob("*.jsonl"))
        self.assertEqual(files, ["captures/A308/2026-10/2026-10-23-hostA.jsonl",
                                 "captures/A308/2026-10/2026-10-23-hostB.jsonl"])
        text = "".join(p.read_text() for p in (stores[1] / "captures").rglob("*.jsonl"))
        for private in ("someone", "HOSTNAME", "private-worktree", ".log"):
            self.assertNotIn(private, text)
        self.assertEqual(self.sync(stores[1], "hostB"), 0)
        self.assertEqual(sum(1 for _ in text.splitlines()), 2)

    def test_a_held_store_is_left_alone(self):
        store = self.dir / "store"
        store.mkdir()
        git(store, "init", "-q")
        (store / ".git" / "perf_index.lock").write_text("")
        self.records.capture(A308, "a1-diag", "2026-10-23T10:00:00", target(ROW, 100, 200))
        self.assertEqual(self.sync(store, "hostA"), 0)
        self.assertFalse((store / "captures").exists())

    def test_hook_starts_a_sync_only_on_release(self):
        with patch.object(perf_index.subprocess, "Popen") as popen:
            for event, started in (("acquired", False), ("released", True)):
                with patch.dict(perf_index.os.environ, {perf_index.LOCK_EVENT_VAR: event}):
                    perf_index.main(["sync", "--hook", "--host", "hostA"])
                self.assertEqual(popen.called, started)
        self.assertNotIn("--hook", popen.call_args[0][0])

    def test_store_starts_an_empty_branch_when_the_remote_has_none(self):
        remote = self.dir / "remote.git"
        subprocess.run(["git", "init", "-q", "--bare", str(remote)], check=True, stdin=subprocess.DEVNULL)
        store = self.dir / "fresh"
        with patch.object(perf_index, "REPO", self.dir / "repo"):
            (self.dir / "repo").mkdir()
            git(self.dir / "repo", "init", "-q")
            git(self.dir / "repo", "remote", "add", "origin", str(remote))
            perf_index.main(["store", "--path", str(store)])
        head = subprocess.run(["git", "-C", str(store), "symbolic-ref", "HEAD"], capture_output=True,
                              text=True, stdin=subprocess.DEVNULL).stdout.strip()
        self.assertEqual(head, "refs/heads/" + perf_index.BRANCH)

    def test_host_must_be_a_short_name(self):
        with self.assertRaises(SystemExit):
            self.sync(self.dir / "store", "JV.local/user")


if __name__ == "__main__":
    unittest.main()
