"""Tests for scripts/lookup/why.py over a throwaway git history.

The fixture is a main line of squash commits `subject (#N)` and one merge
commit `Merge pull request #N`, built fresh per test class; the pull request
cache and the lookup log are throwaway folders.

    python -m unittest discover -s scripts/lookup/tests
"""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import why  # noqa: E402  (puts the lookup kit on the path)
import query_log  # noqa: E402

ENV = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
       "GIT_COMMITTER_EMAIL": "t@t", "GIT_CONFIG_NOSYSTEM": "1", "HOME": tempfile.gettempdir()}


def run(root, *args, day=1):
    date = f"2026-10-{day:02d}T12:00:00+00:00"
    subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True,
                   env={**os.environ, **ENV, "GIT_AUTHOR_DATE": date, "GIT_COMMITTER_DATE": date})


def commit(root, files, message, day):
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    run(root, "add", "-A", day=day)
    run(root, "commit", "-q", "-m", message, day=day)


def make_history():
    """main: #1 adds the file, #2 rewrites line 2, #3 merges a branch that
    rewrites line 3, #4 adds a line that #5 takes out again, and #6 renames
    the file without touching a line."""
    root = Path(tempfile.mkdtemp())
    run(root, "init", "-q", "-b", "main")
    commit(root, {"src/a.c": "int alpha;\nint beta;\nint gamma;\n"}, "a: first cut (#1)", 1)
    commit(root, {"src/a.c": "int alpha;\nint beta_frame(void);\nint gamma;\n"},
           "a: beta becomes a frame call (autana-b2b2) (#2)", 2)
    run(root, "checkout", "-q", "-b", "topic", day=3)
    commit(root, {"src/a.c": "int alpha;\nint beta_frame(void);\nint gamma_frame(void);\n"},
           "wip on gamma", 3)
    run(root, "checkout", "-q", "main", day=3)
    run(root, "merge", "-q", "--no-ff", "topic", "-m",
        "Merge pull request #3 from someone/topic\n\ngamma follows beta (c3c3)", day=4)
    commit(root, {"src/a.c": "int alpha;\nint beta_frame(void);\nint gamma_frame(void);\nint delta;\n"},
           "a: delta (#4)", 5)
    commit(root, {"src/a.c": "int alpha;\nint beta_frame(void);\nint gamma_frame(void);\n"},
           "a: no delta after all (#5)", 6)
    run(root, "mv", "src/a.c", "src/frame.c", day=7)
    run(root, "commit", "-q", "-m", "a: the file is named for what it holds (#6)", day=7)
    return root


class History(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = make_history()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def setUp(self):
        self.cache = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.cache, True)
        for patch in (mock.patch.object(query_log, "log_dir", return_value=self.cache / "usage"),
                      mock.patch.object(why, "pr_cache", return_value=self.cache / "pr")):
            patch.start()
            self.addCleanup(patch.stop)

    def reply(self, query):
        return why.answer(self.root, "HEAD", query, offline=True, with_tickets=False)

    def prs(self, rows):
        return [row["pr"] for row in rows]

    def test_a_file_ranks_the_prs_that_own_its_lines_newest_first(self):
        reply = self.reply("src/frame.c")
        self.assertEqual(self.prs(reply["rows"]), [3, 2, 1])
        self.assertEqual([row["lines"] for row in reply["rows"]], [1, 1, 1])
        self.assertEqual(reply["lines"], 3)

    def test_a_merged_branch_lands_on_its_merge_commit_with_the_pr_title(self):
        row = next(r for r in self.reply("src/frame.c:3")["rows"])
        self.assertEqual((row["pr"], row["title"], row["tickets"]), (3, "gamma follows beta (c3c3)", ["c3c3"]))

    def test_a_line_range_answers_for_those_lines_only(self):
        self.assertEqual(self.prs(self.reply("src/frame.c:2")["rows"]), [2])
        self.assertEqual(self.prs(self.reply("src/frame.c:1-2")["rows"]), [2, 1])

    def test_prs_that_own_no_line_now_follow_newest_first_through_the_rename(self):
        self.assertEqual(self.prs(self.reply("src/frame.c")["history"]), [6, 5, 4])

    def test_a_name_ranks_the_lines_that_name_it_and_the_pr_that_added_it(self):
        reply = self.reply("beta_frame")
        self.assertEqual(reply["sites"], [("src/frame.c", 2)])
        self.assertEqual(self.prs(reply["rows"]), [2])
        self.assertEqual(reply["rows"][0]["tickets"], ["autana-b2b2"])

    def test_a_name_no_longer_in_the_tree_still_finds_the_pr_that_added_it(self):
        reply = self.reply("delta")
        self.assertEqual((reply["lines"], self.prs(reply["rows"]), self.prs(reply["history"])), (0, [4], [5]))

    def test_a_folder_counts_the_lines_each_pr_changed_in_it(self):
        reply = self.reply("src")
        self.assertEqual(reply["kind"], "folder")
        self.assertEqual({row["pr"]: row["lines"] for row in reply["rows"]}, {6: 0, 5: 1, 4: 1, 3: 2, 2: 2, 1: 3})

    def test_the_cached_pr_body_gives_the_summary_and_its_ticket_line(self):
        (self.cache / "pr").mkdir()
        (self.cache / "pr" / "2.json").write_text(json.dumps(
            {"title": "t", "body": "## Summary\n\nBeta is called once per frame. More after.\n\n"
                                   "| a | b |\n\nTicket: autana-x9x9"}), encoding="utf-8")
        row = self.reply("src/frame.c:2")["rows"][0]
        self.assertEqual(row["summary"], "Beta is called once per frame. More after.")
        self.assertEqual(row["tickets"], ["autana-b2b2", "autana-x9x9"])

    def test_offline_without_a_cached_body_keeps_the_subject_and_runs_no_gh(self):
        with mock.patch.object(why.subprocess, "run", wraps=subprocess.run) as spawned:
            row = self.reply("src/frame.c:2")["rows"][0]
        self.assertNotIn("summary", row)
        self.assertFalse([c for c in spawned.call_args_list if "gh" in Path(str(c.args[0][0])).stem])

    def test_the_text_answer_stops_at_the_budget_and_says_how_much_is_left(self):
        reply = self.reply("src/frame.c")
        whole = why.render(reply, 10_000, 5)
        cut = why.render(reply, len(whole[0]) + len(whole[1]) + 2, 5)
        self.assertEqual(cut[:2], whole[:2])
        self.assertEqual(cut[-1], "+5 more (--budget, --history or --json)")
        self.assertTrue(whole[-1].startswith("history: #6 2026-10-07"))

    def test_a_query_nothing_matches_exits_1_and_logs_a_weak_lookup(self):
        with mock.patch("sys.stdout", io.StringIO()) as out:
            code = why.main(["no_such_thing", "--offline"], root=self.root)
        self.assertEqual(code, 1)
        self.assertIn("no line or commit", out.getvalue())
        row, = query_log.read(self.cache / "usage")
        self.assertEqual((row["tool"], row["query"], row["weak"]), ("why", "no_such_thing", True))

    def test_eval_ranks_a_pr_or_a_ticket_among_rows_then_history(self):
        reply = self.reply("src/frame.c")
        self.assertEqual(why.eval_rank(reply, "#1"), 3)
        self.assertEqual(why.eval_rank(reply, "#5"), 5)
        self.assertEqual(why.eval_rank(reply, "b2b2"), 2)
        self.assertIsNone(why.eval_rank(reply, "#9"))


class Tickets(unittest.TestCase):
    def tickets(self, subject):
        return why.Commit("0" * 40, "2026-10-01", subject, "").tickets

    def test_the_subjects_last_parenthesis_names_its_tickets_and_milestones_are_dropped(self):
        self.assertEqual(self.tickets("a: b (autana-ks4hn) (#1)"), ["autana-ks4hn"])
        self.assertEqual(self.tickets("a: b (cvt.16 M1) (#2)"), ["cvt.16"])
        self.assertEqual(self.tickets("a: b (ems.18, hacgs) (#3)"), ["ems.18", "hacgs"])

    def test_a_parenthesis_of_ordinary_words_names_no_ticket(self):
        self.assertEqual(self.tickets("bake: key the bakes (cached bakes, lock) (#4)"), [])
        self.assertEqual(self.tickets("render: small fill (-7.8% fitted-full) (#5)"), [])


class Summary(unittest.TestCase):
    def test_a_long_paragraph_is_cut_at_a_sentence_end(self):
        first = "A" * 200 + ". "
        text = why.summary(first + "B" * 200 + ".")
        self.assertEqual(text, first.strip())

    def test_headings_tables_lists_and_ticket_lines_are_skipped(self):
        self.assertEqual(why.summary("# T\n\n| a |\n\n- x\n\nTicket: autana-q1q1"), "")


if __name__ == "__main__":
    unittest.main()
