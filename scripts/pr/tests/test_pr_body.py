"""The PR description scaffold fills what the diff shows and its check catches what is left."""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import pr_body  # noqa: E402


def git(repo, *args):
    subprocess.run(['git', '-c', 'user.name=t', '-c', 'user.email=t@t', *args], cwd=repo,
                   check=True, capture_output=True)


def mermaid_runs():
    node = shutil.which('node')
    return node is not None and subprocess.run(
        [node, str(pr_body.MERMAID_GATE), '--probe'], capture_output=True).returncode == 0


class Repo:
    """A scratch repository with `base` holding OLD_FILES and HEAD the edits."""

    def __init__(self, old, new, renames=()):
        self.dir = tempfile.TemporaryDirectory()
        self.path = pathlib.Path(self.dir.name)
        git(self.path, 'init', '-q', '-b', 'feature/x')
        for name, text in old.items():
            self.write(name, text)
        git(self.path, 'add', '-A')
        git(self.path, 'commit', '-q', '-m', 'base', '--allow-empty')
        git(self.path, 'tag', 'base')
        for old_name, new_name in renames:
            (self.path / new_name).parent.mkdir(parents=True, exist_ok=True)
            git(self.path, 'mv', old_name, new_name)
        for name, text in new.items():
            if text is None:
                (self.path / name).unlink()
            else:
                self.write(name, text)
        git(self.path, 'add', '-A')
        git(self.path, 'commit', '-q', '-m', 'head')

    def write(self, name, text):
        (self.path / name).parent.mkdir(parents=True, exist_ok=True)
        (self.path / name).write_text(text, encoding='utf-8')

    def __enter__(self):
        self.patch = mock.patch.object(pr_body, 'REPO', self.path)
        self.patch.start()
        return self

    def __exit__(self, *exc):
        self.patch.stop()
        self.dir.cleanup()


class GroupTests(unittest.TestCase):
    def test_few_files_are_their_own_rows(self):
        self.assertEqual(list(pr_body.group(['a/b/c.c', 'd.md'])), ['`a/b/c.c`', '`d.md`'])

    def test_rows_never_exceed_the_limit(self):
        paths = [f'top{t}/mid{m}/f{f}.c' for t in range(3) for m in range(5) for f in range(4)]
        rows = pr_body.group(paths)
        self.assertLessEqual(len(rows), pr_body.MAX_ROWS)
        self.assertEqual(sorted(p for members in rows.values() for p in members), sorted(paths))

    def test_the_largest_directory_is_split_first(self):
        paths = [f'big/sub{s}/f{f}.c' for s in range(3) for f in range(5)] + ['small/a.c', 'small/b.c']
        self.assertEqual(list(pr_body.group(paths)), [
            '`big/sub0/` (5 files)', '`big/sub1/` (5 files)', '`big/sub2/` (5 files)',
            '`small/` (2 files)'])


class ScaffoldTests(unittest.TestCase):
    def test_change_fills_rows_lines_and_ticket(self):
        with Repo({'src/a.c': 'one\n'}, {'src/a.c': 'one\ntwo\n', 'docs/n.md': 'x\n'}):
            body = pr_body.scaffold('change', 'cvpg', 'base', None)
        self.assertIn('| `docs/n.md` | <!-- fill --> | <!-- fill --> |', body)
        self.assertIn('| `src/a.c` |', body)
        self.assertIn('**Lines:** +2 / −0 (net +2)', body)
        self.assertTrue(body.rstrip().endswith('Ticket: cvpg'))
        self.assertNotIn('<!-- block', body)

    def test_out_writes_utf8_whatever_the_shell(self):
        with Repo({'a.c': 'x\n'}, {'a.c': 'y\n'}) as repo:
            out = repo.path / 'body.md'
            self.assertEqual(pr_body.main(['new', '--ticket', 'autana-cvpg', '--base', 'base', '-o', str(out)]), 0)
            body = out.read_text(encoding='utf-8')
        self.assertIn('**Lines:** +1 / −1 (net +0)', body)
        self.assertTrue(body.rstrip().endswith('Ticket: cvpg'))

    def test_move_lists_renames_by_directory(self):
        old = {f'old/f{i}.c': f'body {i}\n' * 20 for i in range(3)}
        with Repo(old, {}, renames=[(f'old/f{i}.c', f'new/f{i}.c') for i in range(3)]):
            body = pr_body.scaffold('move', 'cvpg', 'base', None)
        self.assertIn('| `old/` (3 files) | `new/` |', body)
        self.assertIn('**Lines:** +0 / −0 (net +0)', body)

    def test_docs_counts_lines_and_words_on_both_sides(self):
        with Repo({'a.md': 'one two\nthree\n', 'gone.md': 'x\n'},
                  {'a.md': 'one\n', 'new.md': 'a b c\n', 'gone.md': None}):
            body = pr_body.scaffold('docs', 'cvpg', 'base', None)
        self.assertIn('| `a.md` | 2 lines, 3 words | 1 lines, 1 words |', body)
        self.assertIn('| `new.md` | — | 1 lines, 3 words |', body)
        self.assertIn('| `gone.md` | 1 lines, 1 words | — |', body)

    def test_perf_tabulates_comparison_rows(self):
        comparison = {'label_a': 'main', 'label_b': 'pr', 'incomplete': False, 'rows': {
            'r3d/frame': {'a': 1000.0, 'b': 900.0, 'ratio': 0.9, 'verdict': 'improved'},
            'sand/step': {'a': 50.0, 'b': None, 'ratio': None, 'verdict': 'not measured'}}}
        with Repo({'a.c': 'x\n'}, {'a.c': 'y\n'}) as repo:
            (repo.path / 'out').mkdir()
            (repo.path / 'out' / 'comparison.json').write_text(json.dumps(comparison))
            body = pr_body.scaffold('perf', 'cvpg', 'base', repo.path / 'out')
        self.assertIn('| Row | main us | pr us | Change | Verdict |', body)
        self.assertIn('| `r3d/frame` | 1000 | 900 | -10.0% | improved |', body)
        self.assertIn('| `sand/step` | 50 | n/a | n/a | not measured |', body)


class TicketTests(unittest.TestCase):
    def test_branch_segment_the_backlog_knows(self):
        known = {'ks4hn': '[{"id": "autana-ks4hn"}]'}

        def bd(command, **_):
            segment = command[2]
            return subprocess.CompletedProcess(command, 0 if segment in known else 1,
                                               known.get(segment, ''), '')

        with mock.patch.object(pr_body, 'git', return_value='feature/ks4hn-hash\n'), \
                mock.patch.object(pr_body.shutil, 'which', return_value='bd'), \
                mock.patch.object(pr_body.subprocess, 'run', side_effect=bd):
            self.assertEqual(pr_body.ticket_from_branch(), 'ks4hn')

    def test_no_backlog_means_no_guess(self):
        with mock.patch.object(pr_body, 'git', return_value='feature/ks4hn-hash\n'), \
                mock.patch.object(pr_body.shutil, 'which', return_value=None):
            self.assertIsNone(pr_body.ticket_from_branch())


class CheckTests(unittest.TestCase):
    def test_a_filled_body_passes(self):
        self.assertEqual(pr_body.check('Does a thing.\n\n| A | B |\n|---|---|\n| 1 | 2 |\n\nTicket: cvpg\n'), [])

    def test_template_as_is_fails_on_every_marker_and_the_ticket(self):
        problems = pr_body.check(pr_body.TEMPLATE.read_text(encoding='utf-8'))
        self.assertEqual(sum('unfilled marker' in p for p in problems), 4)
        self.assertTrue(any(p.startswith('ticket:') for p in problems))

    @unittest.skipUnless(mermaid_runs(), 'no mermaid-cli to render with')
    def test_a_broken_diagram_fails(self):
        good = 'Text.\n\n```mermaid\nflowchart LR\n  a --> b\n```\n\nTicket: cvpg\n'
        self.assertEqual(pr_body.check(good), [])
        problems = pr_body.check(good.replace('a --> b', 'a --> (('))
        self.assertTrue(any(p.startswith('mermaid:') for p in problems), problems)


if __name__ == '__main__':
    unittest.main()
