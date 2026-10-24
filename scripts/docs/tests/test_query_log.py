"""Tests for scripts/docs/query_log.py and the lookups docs_search.py logs.

Every test writes to a throwaway folder in place of the per-user cache.

    python -m unittest discover -s scripts/docs/tests
"""
import io
import json
import os
import sys
import tempfile
import time
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

    def test_a_lookup_carries_the_session_it_ran_under(self):
        with mock.patch.dict("os.environ", {query_log.SESSION_VAR: "s1"}):
            self.run_search("serial", "port")
        with mock.patch.dict("os.environ"):
            os.environ.pop(query_log.SESSION_VAR, None)
            self.run_search("serial", "port")
        first, second = self.rows()
        self.assertEqual(first["session"], "s1")
        self.assertNotIn("session", second)

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
                   (self.folder / "eval" / "history.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual((history[0]["suite"], history[0]["rows"], history[0]["mrr"]), ("docs", 1, 1.0))

    def test_a_question_that_is_not_valid_utf8_is_still_logged(self):
        self.run_search("port" + "\udcff")
        self.assertEqual(self.rows()[0]["query"], "port\udcff")

    def test_a_log_that_cannot_be_written_never_fails_the_lookup(self):
        blocker = self.folder / "file"
        blocker.write_text("x", encoding="utf-8")
        with mock.patch.object(query_log, "log_dir", return_value=blocker / "usage"):
            self.assertEqual(self.run_search("serial"), 0)


    def test_a_search_logs_how_much_it_printed(self):
        out = io.StringIO()
        with mock.patch("sys.stdout", out):
            docs_search.main(["serial", "port"], root=self.root)
        self.assertEqual(self.rows()[0]["chars"], len(out.getvalue()))

    def test_an_unknown_word_names_the_nearest_indexed_words(self):
        out = io.StringIO()
        with mock.patch("sys.stdout", out):
            docs_search.main(["serail"], root=self.root)
        self.assertIn("no document uses: serail (nearest: serial)", out.getvalue())


class Lookup(unittest.TestCase):
    def setUp(self):
        self.folder = Path(tempfile.mkdtemp())
        patch = mock.patch.object(query_log, "log_dir", return_value=self.folder)
        patch.start()
        self.addCleanup(patch.stop)

    def test_a_lookup_logs_what_it_printed_found_and_judged(self):
        out = io.StringIO()
        with mock.patch("sys.stdout", out), query_log.lookup("tool", "search", "words") as call:
            call.print("first")
            call.print("rest", end="")
            call.hits = [("a.md:1", 0.5)]
            call.weak = True
            call.fields["semantic"] = False
        row, = query_log.read(self.folder)
        self.assertEqual((row["tool"], row["kind"], row["query"]), ("tool", "search", "words"))
        self.assertEqual(row["chars"], len(out.getvalue()))
        self.assertEqual((row["hits"], row["weak"], row["semantic"]), ([["a.md:1", 0.5]], True, False))
        self.assertIsInstance(row["ms"], int)

    def test_a_lookup_that_raises_is_not_logged(self):
        with self.assertRaises(KeyError), query_log.lookup("tool", "search", "words"):
            raise KeyError("x")
        self.assertEqual(query_log.read(self.folder), [])

    def test_the_session_comes_from_the_first_set_variable(self):
        settings = {"session_vars": ["OUTER_ID", "OTHER_ID"]}
        with mock.patch.object(query_log.autana_config, "load", return_value=settings):
            for environment, expected in (({"OTHER_ID": "b"}, "b"),
                                          ({"OUTER_ID": "a", "OTHER_ID": "b"}, "a"),
                                          ({query_log.SESSION_VAR: "s", "OUTER_ID": "a"}, "s"),
                                          ({}, None)):
                with self.subTest(environment=environment), mock.patch.dict("os.environ", environment, clear=True):
                    self.assertEqual(query_log.session(), expected)

    def test_a_broken_settings_file_leaves_only_the_fixed_variable(self):
        broken = mock.patch.object(query_log.autana_config, "load",
                                   side_effect=query_log.autana_config.ConfigError("bad"))
        with broken, mock.patch.dict("os.environ", {"OUTER_ID": "a"}, clear=True):
            self.assertIsNone(query_log.session())

    def test_nearest_words_are_close_spellings_only(self):
        words = ["serial", "series", "console", "usb"]
        self.assertEqual(query_log.nearest("serail", words), ["serial"])
        self.assertEqual(query_log.nearest("bluetooth", words), [])


class Scores(unittest.TestCase):
    def setUp(self):
        self.folder = Path(tempfile.mkdtemp())
        patch = mock.patch.object(query_log, "log_dir", return_value=self.folder)
        patch.start()
        self.addCleanup(patch.stop)

    def test_mean_reciprocal_rank_and_shares(self):
        result = query_log.scores([("a", 1), ("b", 2), ("c", 4), ("d", None)])
        self.assertEqual(result, {"rows": 4, "mrr": round((1 + 0.5 + 0.25) / 4, 3),
                                  "top1": 0.25, "top3": 0.5})

    def test_a_score_is_kept_per_tool_and_compared_with_the_same_tool_only(self):
        out = io.StringIO()
        with mock.patch("sys.stdout", out):
            query_log.keep_score("hwq", "lexical", [("a", 1), ("b", 2)], time.perf_counter())
            query_log.keep_score("docs", "lexical", [("a", 1), ("b", 2)], time.perf_counter())
            query_log.keep_score("hwq", "lexical", [("a", 1), ("b", None)], time.perf_counter(), facts=2)
        history = [json.loads(line) for line in query_log.eval_history().read_text(encoding="utf-8").splitlines()]
        self.assertEqual([h["suite"] for h in history], ["hwq", "docs", "hwq"])
        self.assertEqual((history[2]["mrr"], history[2]["facts"]), (0.5, 2))
        last = out.getvalue().splitlines()
        self.assertIn("hwq lexical: MRR 0.500, top-1 50%, top-3 50% of 2 questions", last[-2])
        self.assertIn("last ", last[-1])
        self.assertIn("MRR 0.750", last[-1])


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
         "query": "docs/Big.md", "hits": [], "weak": True},
    ]

    def test_counts_followthrough_repeats_misses_and_refusals(self):
        text = query_log.summary(self.ROWS)
        self.assertIn("5 lookups, 2026-10-01 to 2026-10-03", text)
        self.assertRegex(text, r"docs\s+search\s+3\s+33%\s+50%\s+50\s+-")
        self.assertIn("2  docs search: lock protocol", text)
        missed = text.split("missed")[1]
        self.assertIn("1  docs search: bluetooth pairing", missed)
        self.assertIn("1  read-hook bypass: docs/big.md", missed)

    def test_output_is_totalled_in_tokens_per_tool_and_kind(self):
        rows = [dict(self.ROWS[0], chars=4000), dict(self.ROWS[1], chars=2000), self.ROWS[2]]
        tokens = 6000 // query_log.CHARS_PER_TOKEN
        self.assertRegex(query_log.summary(rows), rf"docs\s+search\s+3\s+\S+\s+\S+\s+\S+\s+{tokens}\b")

    def test_session_filter_and_count(self):
        rows = [dict(self.ROWS[0], session="a"), dict(self.ROWS[1], session="b"), self.ROWS[2]]
        self.assertIn("3 lookups, 2026-10-01 to 2026-10-03, 2 sessions", query_log.summary(rows))
        folder = Path(tempfile.mkdtemp())
        (folder / "a.jsonl").write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
        out = io.StringIO()
        with mock.patch.object(query_log, "log_dir", return_value=folder), mock.patch("sys.stdout", out):
            query_log.main(["--session", "b"])
        self.assertIn("1 lookups", out.getvalue())

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
