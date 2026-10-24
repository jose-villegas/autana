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

    def test_a_line_ranges_history_is_the_history_of_those_lines(self):
        history = self.prs(self.reply("src/frame.c:2")["history"])
        self.assertIn(1, history)
        self.assertFalse({3, 4, 5} & set(history))

    def test_a_path_resolves_from_the_folder_the_command_runs_in(self):
        reply = why.answer(self.root, "HEAD", "frame.c:2", offline=True, with_tickets=False, prefix="src/")
        self.assertEqual((reply["kind"], self.prs(reply["rows"])), ("range", [2]))
        reply = why.answer(self.root, "HEAD", "src/frame.c:2", offline=True, with_tickets=False, prefix="src/")
        self.assertEqual(reply["kind"], "range")

    def test_a_name_ranks_the_lines_that_name_it_and_the_pr_that_added_it(self):
        reply = self.reply("beta_frame")
        self.assertEqual(reply["sites"], [("src/frame.c", 2)])
        self.assertEqual(self.prs(reply["rows"]), [2])
        self.assertEqual(reply["rows"][0]["tickets"], ["autana-b2b2"])

    def test_a_name_no_longer_in_the_tree_still_finds_the_pr_that_added_it(self):
        reply = self.reply("delta")
        self.assertEqual((reply["lines"], self.prs(reply["rows"]), self.prs(reply["history"])), (0, [4], [5]))

    def test_commit_shas_go_to_git_on_stdin_so_a_big_folder_fits_any_command_line(self):
        shas = set(why.history(self.root, "HEAD", "--", "src"))
        with mock.patch.object(why.subprocess, "run", wraps=subprocess.run) as spawned:
            found = why.commits(self.root, shas)
        self.assertEqual(set(found), shas)
        argv = " ".join(" ".join(c.args[0]) for c in spawned.call_args_list)
        self.assertFalse([sha for sha in shas if sha in argv])

    def test_a_folder_counts_the_lines_each_pr_changed_in_it(self):
        reply = self.reply("src")
        self.assertEqual(reply["kind"], "folder")
        self.assertEqual({row["pr"]: row["lines"] for row in reply["rows"]}, {6: 0, 5: 1, 4: 1, 3: 2, 2: 2, 1: 3})

    def test_the_cached_pr_body_gives_the_summary_and_its_ticket_line(self):
        # The fixture has no origin remote, so its pull requests file under "local".
        (self.cache / "pr" / "local").mkdir(parents=True)
        (self.cache / "pr" / "local" / "2.json").write_text(json.dumps(
            {"title": "t", "body": "## Summary\n\nBeta is called once per frame. More after.\n\n"
                                   "| a | b |\n\nTicket: autana-x9x9"}), encoding="utf-8")
        row = self.reply("src/frame.c:2")["rows"][0]
        self.assertEqual(row["summary"], "Beta is called once per frame. More after.")
        self.assertEqual(row["tickets"], ["autana-b2b2", "autana-x9x9"])

    def test_a_ticket_line_typed_on_github_with_crlf_ends_still_counts(self):
        (self.cache / "pr" / "local").mkdir(parents=True)
        (self.cache / "pr" / "local" / "2.json").write_text(
            json.dumps({"title": "t", "body": "Beta.\r\n\r\nTicket: q1q1\r\n"}), encoding="utf-8")
        self.assertEqual(self.reply("src/frame.c:2")["rows"][0]["tickets"], ["autana-b2b2", "q1q1"])

    def test_a_ticket_named_in_subject_and_ticket_line_appears_once(self):
        (self.cache / "pr" / "local").mkdir(parents=True)
        (self.cache / "pr" / "local" / "2.json").write_text(
            json.dumps({"title": "t", "body": "Beta.\n\nTicket: autana-b2b2"}), encoding="utf-8")
        self.assertEqual(self.reply("src/frame.c:2")["rows"][0]["tickets"], ["autana-b2b2"])

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
        self.assertEqual(cut[-1], "+2 more PRs and 3 older (--budget, --history or --json)")
        self.assertTrue(whole[-1].startswith("history: #6 2026-10-07"))
        self.assertEqual(why.render(reply, 10_000, 1)[-1], "+2 older (--budget, --history or --json)")

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


class Scratch(unittest.TestCase):
    """A fixture history built once per class by build(), with a throwaway cache and log."""

    @classmethod
    def setUpClass(cls):
        cls.root = cls.build()

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

    def cache_body(self, number, body):
        (self.cache / "pr" / "local").mkdir(parents=True, exist_ok=True)
        (self.cache / "pr" / "local" / f"{number}.json").write_text(
            json.dumps({"title": "t", "body": body}), encoding="utf-8")


def new_repo():
    root = Path(tempfile.mkdtemp())
    run(root, "init", "-q", "-b", "main")
    return root


class MergeOrder(Scratch):
    @staticmethod
    def build():
        """Merge order is #7, #3, #5 (a merge commit), #4: neither number order."""
        root = new_repo()
        commit(root, {"f.c": "l1\n"}, "f: one (#7)", 1)
        commit(root, {"f.c": "l1\nl2\n"}, "f: two (#3)", 2)
        run(root, "checkout", "-q", "-b", "topic", day=3)
        commit(root, {"f.c": "l1\nl2\nl3\n"}, "wip", 3)
        run(root, "checkout", "-q", "main", day=3)
        run(root, "merge", "-q", "--no-ff", "topic", "-m", "Merge pull request #5 from x/topic\n\nthree", day=4)
        commit(root, {"f.c": "l1\nl2\nl3\nl4\n"}, "f: four (#4)", 5)
        return root

    def test_rows_come_out_newest_merge_first_not_by_pr_number(self):
        self.assertEqual(self.prs(self.reply("f.c")["rows"]), [4, 5, 3, 7])


class NameSites(Scratch):
    @staticmethod
    def build():
        root = new_repo()
        commit(root, {"include/x.h": "int beta;\nint beta;\nint beta_frame;\n", "src/b.c": "beta();\n",
                      "docs/a.md": "beta is\n", "docs/d.md": "beta too\n"}, "names (#1)", 1)
        return root

    def test_a_name_matches_whole_words_only(self):
        sites = why.name_sites(self.root, "HEAD", "beta")
        self.assertNotIn(("include/x.h", 3), sites)
        self.assertEqual(len(sites), 5)

    def test_shown_sites_lead_with_the_header_one_per_file_at_most_the_cap(self):
        reply = self.reply("beta")
        self.assertEqual(reply["sites"], [("include/x.h", 1), ("src/b.c", 1), ("docs/a.md", 1)])
        self.assertEqual(len(reply["sites"]), why.SITES_SHOWN)

    def test_site_count_counts_every_line_that_names_it(self):
        self.assertEqual(self.reply("beta")["site_count"], 5)


class Capped(Scratch):
    @staticmethod
    def build():
        root = new_repo()
        lines = ""
        for n in range(1, why.FULL_ROWS + 2):
            lines += f"line {n}\n"
            commit(root, {"f.c": lines}, f"f: line {n} (#{n})", n)
        return root

    def test_only_the_first_full_rows_read_pr_text(self):
        for n in range(1, why.FULL_ROWS + 2):
            self.cache_body(n, f"Body {n}.")
        rows = self.reply("f.c")["rows"]
        self.assertEqual(len(rows), why.FULL_ROWS + 1)
        self.assertEqual([r.get("summary") for r in rows[:why.FULL_ROWS]],
                         [f"Body {n}." for n in range(why.FULL_ROWS + 1, 1, -1)])
        self.assertNotIn("summary", rows[why.FULL_ROWS])


class Reindent(Scratch):
    @staticmethod
    def build():
        root = new_repo()
        commit(root, {"f.c": "if (x) {\n  a();\n}\n"}, "f: first (#1)", 1)
        commit(root, {"f.c": "if (x) {\n    a();\n}\n"}, "f: reindent (#2)", 2)
        return root

    def test_a_pr_that_only_reindents_a_line_does_not_own_it(self):
        self.assertEqual(self.prs(self.reply("f.c:2")["rows"]), [1])


class Render(unittest.TestCase):
    def row(self, pr, title, commit="abcdef123"):
        return {"pr": pr, "commit": commit, "date": "2026-10-01", "title": title, "tickets": [], "lines": 1}

    def reply(self, rows, kind="file", history=(), **extra):
        return {"kind": kind, "target": "f.c", "lines": 3, "sites": [], "site_count": 0,
                "rows": rows, "history": list(history), **extra}

    @staticmethod
    def size(lines):
        return sum(len(x) + 1 for x in lines)

    def test_a_block_that_overflows_stops_the_list_instead_of_skipping_to_a_shorter_one(self):
        reply = self.reply([self.row(1, "short"), self.row(2, "x" * 300), self.row(3, "short")])
        whole = why.render(reply, 10_000, 5)
        out = why.render(reply, self.size([whole[0], whole[1], whole[3]]), 5)
        self.assertTrue(any(x.startswith("#1 ") for x in out))
        self.assertFalse([x for x in out if x.startswith(("#2 ", "#3 "))])

    def test_the_history_line_is_absent_when_rows_were_cut(self):
        old = self.row(9, "old")
        reply = self.reply([self.row(1, "short"), self.row(2, "x" * 300)], history=[old])
        whole = why.render(reply, 10_000, 5)
        self.assertTrue(whole[-1].startswith("history: "))
        out = why.render(reply, self.size([whole[0], whole[1], whole[-1]]) + 5, 5)
        self.assertFalse([x for x in out if x.startswith("history:")])
        self.assertEqual(out[-1], "+1 more PRs and 1 older (--budget, --history or --json)")

    def test_a_main_line_commit_without_a_pr_is_labelled_by_its_sha_prefix(self):
        out = why.render(self.reply([self.row(None, "direct push")]), 10_000, 5)
        self.assertTrue(out[1].startswith("abcdef123 2026-10-01"))

    def test_folder_name_and_file_headers_differ(self):
        rows = [self.row(1, "a"), self.row(2, "b")]
        self.assertEqual(why.render(self.reply(rows, "folder"), 10_000, 5)[0], "f.c  2 PRs changed 3 lines in it")
        self.assertEqual(why.render(self.reply(rows, "file"), 10_000, 5)[0], "f.c  2 PRs own these 3 lines")
        self.assertEqual(why.render(self.reply(rows, "name", site_count=4), 10_000, 5)[0],
                         "f.c  2 PRs own these 3 lines (4 lines name it)")


class PrText(unittest.TestCase):
    def setUp(self):
        self.cache = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.cache, True)
        for patch in (mock.patch.object(why, "pr_cache", return_value=self.cache),
                      mock.patch.object(why, "repo_key", return_value="o-r")):
            patch.start()
            self.addCleanup(patch.stop)

    def gh(self, state):
        reply = subprocess.CompletedProcess([], 0, stdout=json.dumps({"title": "t", "body": "b", "state": state}))
        return mock.patch.object(why.subprocess, "run", return_value=reply)

    def test_a_merged_reply_is_cached_and_the_second_call_runs_no_gh(self):
        with self.gh("MERGED") as spawned:
            first = why.pr_text(self.cache, 7, False)
            second = why.pr_text(self.cache, 7, False)
        self.assertEqual(spawned.call_count, 1)
        self.assertEqual(first, second)
        self.assertTrue((self.cache / "o-r" / "7.json").is_file())

    def test_an_open_reply_is_returned_but_not_cached(self):
        with self.gh("OPEN") as spawned:
            self.assertEqual(why.pr_text(self.cache, 7, False)["state"], "OPEN")
            why.pr_text(self.cache, 7, False)
        self.assertEqual(spawned.call_count, 2)
        self.assertFalse((self.cache / "o-r" / "7.json").exists())

    def test_gh_failing_or_timing_out_gives_none(self):
        failed = subprocess.CompletedProcess([], 1, stdout="")
        for effect in ({"return_value": failed}, {"side_effect": OSError("no gh")},
                       {"side_effect": subprocess.TimeoutExpired("gh", 1)}):
            with mock.patch.object(why.subprocess, "run", **effect):
                self.assertIsNone(why.pr_text(self.cache, 7, False))


class RepoKey(unittest.TestCase):
    def key(self, url):
        root = new_repo()
        self.addCleanup(shutil.rmtree, root, True)
        run(root, "remote", "add", "origin", url)
        return why.repo_key(root)

    def test_https_and_ssh_urls_of_one_repo_share_a_key_and_other_repos_differ(self):
        self.assertEqual(self.key("https://github.com/o/r.git"), self.key("git@github.com:o/r"))
        self.assertNotEqual(self.key("https://github.com/o/r.git"), self.key("https://github.com/o/other.git"))
        self.assertNotEqual(self.key("https://github.com/o/r.git"), self.key("https://github.com/p/r.git"))


class Eval(Scratch):
    @staticmethod
    def build():
        return make_history()

    def test_a_question_ranks_from_the_main_line_as_of_the_day_it_was_asked(self):
        table = self.cache / "eval.tsv"
        table.write_text("gamma_frame\t#3\t2026-10-02T23:00:00+00:00\n"
                         "src/a.c\t#2\t2026-10-02T23:00:00+00:00\n"
                         "alpha\t#1\t2026-10-02T23:00:00+00:00\n", encoding="utf-8")
        with mock.patch.object(why, "EVAL_FILE", table), mock.patch("sys.stdout", io.StringIO()) as out:
            why.run_eval(self.root, "HEAD")
        # #3 had not merged yet, so its question finds nothing; the others rank first.
        self.assertEqual([line.split()[0] for line in out.getvalue().splitlines()[:3]], ["-", "1", "1"])
        kept = query_log.scores_of("why")[-1]
        self.assertEqual((kept["mode"], kept["rows"], kept["mrr"], kept["top1"]), ("git", 3, 0.667, 0.667))


class Bd(unittest.TestCase):
    def test_a_missing_bd_bad_json_or_a_timeout_gives_no_tickets(self):
        bad = subprocess.CompletedProcess([], 0, stdout="not json")
        for effect in ({"side_effect": OSError("no bd")}, {"return_value": bad},
                       {"side_effect": subprocess.TimeoutExpired("bd", 1)}):
            with mock.patch.object(why.subprocess, "run", **effect):
                self.assertEqual(why.tickets(["autana-x1y2"]), {})

    def test_a_returned_id_is_found_with_and_without_the_autana_prefix(self):
        issues = [{"id": "autana-x1y2", "status": "open", "title": "T"}]
        done = subprocess.CompletedProcess([], 0, stdout=json.dumps(issues))
        with mock.patch.object(why.subprocess, "run", return_value=done):
            found = why.tickets(["x1y2"])
        self.assertEqual(found["x1y2"], found["autana-x1y2"])
        self.assertEqual(found["x1y2"]["title"], "T")


class Tickets(unittest.TestCase):
    def tickets(self, subject):
        return why.Commit("0" * 40, "0", "2026-10-01", subject, "").tickets

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

    def test_a_long_paragraph_with_no_early_sentence_end_ends_in_an_ellipsis(self):
        for text in ("word " * 100, "A. " + "B" * 400):
            cut = why.summary(text)
            self.assertTrue(cut.endswith(" ..."), cut)
            self.assertLessEqual(len(cut), why.SUMMARY_CHARS + 4)


if __name__ == "__main__":
    unittest.main()
