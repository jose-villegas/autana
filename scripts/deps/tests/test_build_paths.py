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

    def test_replaces_in_tree_folder(self):
        link = self.launcher / 'build.dev'
        link.mkdir()
        (link / 'old').write_text('output')
        target = build_paths.prepare(self.launcher, 'build.dev')
        self.assertEqual(link.resolve(), target)
        self.assertFalse((target / 'old').exists())

    def test_remove_populated_link_preserves_target(self):
        target = build_paths.prepare(self.launcher, 'build.dev')
        (target / 'sentinel').write_text('keep')
        link = self.launcher / 'build.dev'
        build_paths.remove_output(link)
        self.assertFalse(link.exists())
        self.assertEqual((target / 'sentinel').read_text(), 'keep')

    def test_repoint_preserves_previous_target(self):
        target = build_paths.prepare(self.launcher, 'build.dev')
        (target / 'sentinel').write_text('keep')
        other = self.root / 'other'
        (self.project / 'autana.local.toml').write_text(f"build_root = '{other.as_posix()}'\n")
        new = build_paths.prepare(self.launcher, 'build.dev')
        self.assertNotEqual(new, target)
        self.assertEqual((self.launcher / 'build.dev').resolve(), new)
        self.assertEqual((target / 'sentinel').read_text(), 'keep')

    def test_prune_orphan(self):
        self.build_root.mkdir()
        orphan = self.build_root / 'deadbeef'
        orphan.mkdir()
        (orphan / build_paths.OWNER_FILE).write_text(str(self.root / 'missing/launcher'))
        build_paths.prepare(self.launcher, 'build.diag')
        self.assertFalse(orphan.exists())

    def test_prune_keeps_live_owner(self):
        folder = self.build_root / 'deadbeef'
        folder.mkdir(parents=True)
        (folder / build_paths.OWNER_FILE).write_text(str(self.launcher))
        build_paths.prepare(self.launcher, 'build.diag')
        self.assertTrue(folder.exists())

    def test_prune_keeps_unowned_folder(self):
        folder = self.build_root / 'deadbeef'
        folder.mkdir(parents=True)
        build_paths.prepare(self.launcher, 'build.dev')
        self.assertTrue(folder.exists())

    def test_prune_keeps_non_hash_name(self):
        folder = self.build_root / 'other'
        folder.mkdir(parents=True)
        (folder / build_paths.OWNER_FILE).write_text(str(self.root / 'missing'))
        build_paths.prepare(self.launcher, 'build.dev')
        self.assertTrue(folder.exists())

    def test_prune_keeps_link(self):
        target = build_paths.prepare(self.launcher, 'build.dev')
        (target / build_paths.OWNER_FILE).write_text(str(self.root / 'missing'))
        link = self.build_root / 'deadbeef'
        if os.name == 'nt':
            import _winapi
            _winapi.CreateJunction(str(target), str(link))
        else:
            link.symlink_to(target, target_is_directory=True)
        build_paths.prepare(self.launcher, 'build.diag')
        self.assertTrue(link.exists())

    def test_invalid_name_refused_before_writes(self):
        with self.assertRaises(ValueError):
            build_paths.prepare(self.launcher, '../outside')
        self.assertFalse(self.build_root.exists())

    def test_hash_collision_refused(self):
        target = build_paths.prepare(self.launcher, 'build.dev')
        (target.parent / build_paths.OWNER_FILE).write_text(str(self.root / 'other'))
        with self.assertRaisesRegex(ValueError, 'collision'):
            build_paths.prepare(self.launcher, 'build.dev')

    def test_matching_stamp_keeps_output(self):
        build = build_paths.prepare(self.launcher, 'build.dev')
        selected = {'commit': 'a', 'path': str(self.launcher)}
        build_paths.stamp(build, selected)
        (build / 'sentinel').touch()
        build_paths.reconfigure(build, selected)
        self.assertTrue((build / 'sentinel').exists())

    def test_stamp_mismatch_wipes(self):
        build = build_paths.prepare(self.launcher, 'build.dev')
        selected = {'commit': 'a', 'path': str(self.launcher)}
        build_paths.stamp(build, selected)
        (build / 'sentinel').touch()
        build_paths.reconfigure(build, {**selected, 'commit': 'b'})
        self.assertFalse((build / 'sentinel').exists())

    def test_unstamped_output_wiped_once(self):
        build = build_paths.prepare(self.launcher, 'build.dev')
        selected = {'commit': 'a', 'path': str(self.launcher)}
        (build / 'sentinel').touch()
        build_paths.reconfigure(build, selected)
        self.assertFalse((build / 'sentinel').exists())
        build_paths.stamp(build, selected)
        (build / 'sentinel').touch()
        build_paths.reconfigure(build, selected)
        self.assertTrue((build / 'sentinel').exists())

    def test_wipe_preserves_log_and_lock(self):
        build = build_paths.prepare(self.launcher, 'build.dev')
        preserved = {'build.log', build_paths.BUILD_LOCK}
        for name in preserved:
            (build / name).write_text('keep')
        (build / 'sentinel').touch()
        build_paths.reconfigure(build, {'commit': 'a', 'path': str(self.launcher)})
        self.assertEqual({p.name for p in build.iterdir()}, preserved)

    def test_real_gitignore_ignores_link(self):
        subprocess.run(['git', 'init', '-q', str(self.project)], check=True)
        source = Path(__file__).resolve().parents[3] / '.gitignore'
        (self.project / '.gitignore').write_bytes(source.read_bytes())
        build_paths.prepare(self.launcher, 'build.dev')
        result = subprocess.run(['git', '-C', str(self.project), 'check-ignore', 'launcher/build.dev'], capture_output=True)
        self.assertEqual(result.returncode, 0)

    @unittest.skipUnless(os.name == 'nt', 'Windows cache default')
    def test_default_windows_root(self):
        from unittest import mock
        (self.project / 'autana.local.toml').unlink()
        with mock.patch.dict(os.environ, {'LOCALAPPDATA': str(self.root / 'local')}):
            target = build_paths.prepare(self.launcher, 'build.dev')
        self.assertEqual(target.parents[1], self.root / 'local/autana/b')

    @unittest.skipUnless(os.name == 'nt', 'Windows junction worktree removal')
    def test_git_worktree_remove_preserves_target(self):
        git = ['git', '-c', 'user.name=test', '-c', 'user.email=test@example.org']
        repo = self.root / 'repo'
        subprocess.run([*git, 'init', '-q', str(repo)], check=True)
        (repo / '.gitignore').write_bytes((Path(__file__).resolve().parents[3] / '.gitignore').read_bytes())
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
