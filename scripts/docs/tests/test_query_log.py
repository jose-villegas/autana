"""Tests for scripts/docs/query_log.py and the lookups docs_search.py logs.

Every test writes to a throwaway folder in place of the per-user cache.

    python -m unittest discover -s scripts/docs/tests
"""
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import docs_llama  # noqa: E402
import docs_search  # noqa: E402
import query_log  # noqa: E402


def make_repo(files):
    root = Path(tempfile.mkdtemp())
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return root


class Logged(unittest.TestCase):
    def setUp(self):
        self.folder = Path(tempfile.mkdtemp())
        for patch in (mock.patch.object(query_log, "log_dir", return_value=self.folder),
                      mock.patch.object(docs_llama, "installed", return_value=False)):
            patch.start()
            self.addCleanup(patch.stop)
        self.root = make_repo({"docs/Flashing.md": "# Flashing\n\n## Serial port\n\nThe console is USB.\n"})

    def rows(self):
        return query_log.read(self.folder)

    def run_search(self, *argv):
        with mock.patch("sys.stdout", io.StringIO()):
            return docs_search.main(list(argv), root=self.root)

    def test_a_search_logs_its_question_results_and_verdict(self):
        self.run_search("serial", "port")
        row, = self.rows()
        self.assertEqual((row["tool"], row["kind"], row["query"]), ("docs", "search", "serial port"))
        self.assertEqual(row["hits"][0][0], "docs/Flashing.md:3-5")
        self.assertFalse(row["weak"])
        self.assertFalse(row["semantic"])
        self.assertIsInstance(row["ms"], int)

    def test_a_question_nothing_answers_is_logged_weak(self):
        self.run_search("bluetooth", "pairing")
        self.assertTrue(self.rows()[0]["weak"])

    def test_a_missing_section_is_logged_weak(self):
        self.run_search("--section", "docs/Flashing.md#nowhere")
        row, = self.rows()
        self.assertEqual((row["kind"], row["weak"]), ("section", True))

    def test_eval_logs_no_lookups_but_keeps_its_score(self):
        with mock.patch.object(docs_search, "load_eval",
                               return_value=[("serial port", "docs/Flashing.md", "serial port")]):
            self.run_search("--eval")
        self.assertEqual(self.rows(), [])
        history = [json.loads(line) for line in
                   (self.folder / "eval-history.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual((history[0]["suite"], history[0]["rows"], history[0]["mrr"]), ("docs", 1, 1.0))

    def test_a_log_that_cannot_be_written_never_fails_the_lookup(self):
        blocker = self.folder / "file"
        blocker.write_text("x", encoding="utf-8")
        with mock.patch.object(query_log, "log_dir", return_value=blocker / "usage"):
            self.assertEqual(self.run_search("serial"), 0)


class Scores(unittest.TestCase):
    def test_mean_reciprocal_rank_and_shares(self):
        result = docs_search.scores([("a", 1), ("b", 2), ("c", 4), ("d", None)])
        self.assertEqual(result, {"rows": 4, "mrr": round((1 + 0.5 + 0.25) / 4, 3),
                                  "top1": 0.25, "top3": 0.5})


class Summary(unittest.TestCase):
    ROWS = [
        {"time": "2026-10-01T10:00:00+00:00", "tool": "docs", "kind": "search",
         "query": "lock protocol", "hits": [["docs/a.md:1-9", 0.1]], "weak": False, "ms": 40, "followed": 1},
        {"time": "2026-10-02T10:00:00+00:00", "tool": "docs", "kind": "search",
         "query": "Lock protocol", "hits": [], "weak": False, "ms": 60, "followed": 0},
        {"time": "2026-10-03T10:00:00+00:00", "tool": "docs", "kind": "search",
         "query": "bluetooth pairing", "hits": [], "weak": True, "ms": 50},
        {"time": "2026-10-03T11:00:00+00:00", "tool": "read-hook", "kind": "refused",
         "query": "docs/Big.md", "hits": []},
        {"time": "2026-10-03T11:01:00+00:00", "tool": "read-hook", "kind": "bypass",
         "query": "docs/Big.md", "hits": []},
    ]

    def test_counts_followthrough_repeats_misses_and_refusals(self):
        text = query_log.summary(self.ROWS)
        self.assertIn("5 lookups, 2026-10-01 to 2026-10-03", text)
        self.assertRegex(text, r"docs\s+search\s+3\s+33%\s+50%\s+50")
        self.assertIn("2  docs: lock protocol", text)
        self.assertIn("1  docs: bluetooth pairing", text.split("missed")[1])
        self.assertIn("1  docs/Big.md  (1)", text)

    def test_rows_without_a_tool_are_not_lookups(self):
        folder = Path(tempfile.mkdtemp())
        (folder / "a.jsonl").write_text(json.dumps(self.ROWS[0]) + "\n" + json.dumps({"suite": "docs"})
                                        + "\nnot json\n", encoding="utf-8")
        self.assertEqual(len(query_log.read(folder)), 1)

    def test_days_keeps_only_recent_rows(self):
        self.assertEqual(query_log.since(self.ROWS, 0), self.ROWS)
        self.assertEqual(query_log.since(self.ROWS, 1), [])


if __name__ == "__main__":
    unittest.main()
