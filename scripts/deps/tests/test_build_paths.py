"""Real filesystem coverage for external firmware build folders."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'launcher/tools/build'))
import build_paths


class BuildPathsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.project = self.root / 'project'
        self.launcher = self.project / 'launcher'
        self.launcher.mkdir(parents=True)
        self.build_root = self.root / 'b'
        (self.project / 'autana.local.toml').write_text(f"build_root = '{self.build_root.as_posix()}'\n")

    def tearDown(self):
        self.temp.cleanup()

    def test_links_replace_repoint_and_remove_only_link(self):
        link = self.launcher / 'build.dev'
        link.mkdir()
        (link / 'old').write_text('output')
        target = build_paths.prepare(self.launcher, 'build.dev')
        self.assertEqual(link.resolve(), target)
        (target / 'sentinel').write_text('keep')
        self.assertEqual((link / 'sentinel').read_text(), 'keep')
        self.assertEqual(build_paths.prepare(self.launcher, 'build.dev'), target)
        if os.name == 'nt':
            self.assertTrue(link.is_junction())
            os.rmdir(link)
        else:
            link.unlink()
        self.assertTrue((target / 'sentinel').is_file())
        other = self.root / 'other'
        (self.project / 'autana.local.toml').write_text(f"build_root = '{other.as_posix()}'\n")
        self.assertNotEqual(build_paths.prepare(self.launcher, 'build.dev'), target)
        (self.project / 'autana.local.toml').write_text(f"build_root = '{self.build_root.as_posix()}'\n")
        self.assertEqual(build_paths.prepare(self.launcher, 'build.dev'), target)
        self.assertTrue((target / 'sentinel').is_file())
        orphan = self.build_root / 'deadbeef'
        orphan.mkdir()
        (orphan / 'owner.txt').write_text(str(self.root / 'missing/launcher'))
        with contextlib.redirect_stderr(io.StringIO()) as output:
            build_paths.prepare(self.launcher, 'build.diag')
        self.assertIn('pruned 1 orphan', output.getvalue())
        self.assertFalse(orphan.exists())

    def test_stale_and_matching_stamp(self):
        build = build_paths.prepare(self.launcher, 'build.dev')
        stamp = {'commit': 'a', 'path': str(self.launcher)}
        build_paths.stamp(build, stamp)
        sentinel = build / 'sentinel'
        sentinel.write_text('keep')
        build_paths.reconfigure(build, stamp)
        self.assertTrue(sentinel.exists())
        with contextlib.redirect_stderr(io.StringIO()) as output:
            build_paths.reconfigure(build, {**stamp, 'commit': 'b'})
        self.assertIn('reconfiguring from scratch', output.getvalue())
        self.assertFalse(sentinel.exists())
        build_paths.stamp(build, stamp)
        sentinel.write_text('stale path')
        build_paths.reconfigure(build, {**stamp, 'path': str(self.root)})
        self.assertFalse(sentinel.exists())
        (build / 'CMakeCache.txt').write_text('IDF_PATH:PATH=' + str(self.launcher) + '\n')
        sentinel.write_text('stale')
        build_paths.reconfigure(build, stamp)
        self.assertFalse(sentinel.exists())
        (build / 'project_description.json').write_text(json.dumps({'idf_path': stamp['path'], 'git_revision': 'v6.1-1-ga'}))
        sentinel.write_text('matching legacy output')
        build_paths.reconfigure(build, stamp)
        self.assertTrue(sentinel.exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows junction worktree removal')
    def test_git_worktree_remove_preserves_target(self):
        git = ['git', '-c', 'user.name=test', '-c', 'user.email=test@example.org']
        repo = self.root / 'repo'
        subprocess.run([*git, 'init', '-q', str(repo)], check=True)
        (repo / '.gitignore').write_text('launcher/build*/\nautana.local.toml\n')
        subprocess.run([*git, '-C', str(repo), 'add', '.'], check=True)
        subprocess.run([*git, '-C', str(repo), 'commit', '-qm', 'fixture'], check=True)
        worktree = self.root / 'wt'
        subprocess.run([*git, '-C', str(repo), 'worktree', 'add', '-q', '--detach', str(worktree)], check=True)
        (worktree / 'launcher').mkdir()
        (worktree / 'autana.local.toml').write_text(f"build_root = '{self.build_root.as_posix()}'\n")
        target = build_paths.prepare(worktree / 'launcher', 'build')
        (target / 'sentinel').write_text('keep')
        self.assertEqual(subprocess.check_output(['git', '-C', str(worktree), 'status', '--porcelain'], text=True), '')
        subprocess.run([*git, '-C', str(repo), 'worktree', 'remove', '--force', str(worktree)], check=True)
        self.assertEqual((target / 'sentinel').read_text(), 'keep')

    @unittest.skipUnless(os.name != 'nt', 'Linux default stays in tree')
    def test_default_in_tree(self):
        (self.project / 'autana.local.toml').unlink()
        target = build_paths.prepare(self.launcher, 'build.dev')
        self.assertEqual(target, self.launcher / 'build.dev')
        self.assertFalse(target.is_symlink())


if __name__ == '__main__':
    unittest.main()
