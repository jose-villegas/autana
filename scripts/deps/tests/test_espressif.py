from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'launcher/tools/build'))
import espressif
import json
import os
import subprocess
import tempfile
import unittest


class ResolutionTests(unittest.TestCase):
    def test_real_checkout_and_tools(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project = root / 'project'
            (project / 'launcher').mkdir(parents=True)
            install = root / 'v6.1'
            idf = install / 'esp-idf'
            (idf / 'tools/cmake').mkdir(parents=True)
            (idf / 'tools/cmake/version.cmake').write_text('set(IDF_VERSION_MAJOR 6)\nset(IDF_VERSION_MINOR 1)\n')
            git = ['git', '-c', 'user.name=test', '-c', 'user.email=test@example.org']
            subprocess.run([*git, 'init', '-q', str(idf)], check=True)
            subprocess.run([*git, '-C', str(idf), 'add', '.'], check=True)
            subprocess.run([*git, '-C', str(idf), 'commit', '-qm', 'fixture'], check=True)
            commit = subprocess.check_output(['git', '-C', str(idf), 'rev-parse', 'HEAD'], text=True).strip()
            (project / 'launcher/idf-pin.json').write_text(json.dumps({'commit': commit, 'image': 'espressif/idf:release-v6.1@sha256:test'}))
            tools = install / 'tools'
            (tools / 'python_env/idf6.1_py3.14_env').mkdir(parents=True)
            (project / 'autana.local.toml').write_text(f"idf_installs = ['{install.as_posix()}']\n")
            resolved = espressif.resolve_idf(project, environ={}, roots=[], persistent=lambda key: None)
            self.assertEqual(resolved, (idf.resolve(), tools.resolve()))
            self.assertEqual(espressif.idf_commit(idf), commit)
            subprocess.run(['git', '-C', str(idf), 'pack-refs', '--all'], check=True)
            self.assertEqual(espressif.idf_commit(idf), commit)
            worktree = root / 'idf-worktree'
            subprocess.run(['git', '-C', str(idf), 'worktree', 'add', '-q', '-b', 'fixture-worktree', str(worktree)], check=True)
            self.assertEqual(espressif.idf_commit(worktree), commit)
            subprocess.run(['git', '-C', str(idf), 'worktree', 'remove', str(worktree)], check=True)
            (project / 'autana.local.toml').unlink()
            self.assertEqual(espressif.resolve_idf(project, environ={}, roots=[root], persistent=lambda key: None), resolved)
            with self.assertRaisesRegex(ValueError, 'git clone') as missing:
                espressif.resolve_idf(project, environ={}, roots=[], persistent=lambda key: None)
            self.assertIn(commit, str(missing.exception))
            self.assertIn('submodule update --init --recursive', str(missing.exception))
            self.assertIn('install.', str(missing.exception))
            clean_env = dict(os.environ)
            clean_env.pop('IDF_PATH', None)
            clean_env.pop('IDF_TOOLS_PATH', None)
            failure = subprocess.run([sys.executable, espressif.__file__, 'env', str(project)],
                                     env=clean_env, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(failure.returncode, 0)
            self.assertEqual(failure.stdout, '')
            self.assertIn(commit, failure.stderr)
            self.assertIn('git clone', failure.stderr)
            off = root / 'override'
            subprocess.run(['git', 'clone', '-q', str(idf), str(off)], check=True)
            (off / 'other').write_text('different')
            subprocess.run([*git, '-C', str(off), 'add', '.'], check=True)
            subprocess.run([*git, '-C', str(off), 'commit', '-qm', 'override'], check=True)
            env = {'IDF_PATH': str(off), 'IDF_TOOLS_PATH': str(tools)}
            self.assertEqual(espressif.resolve_idf(project, environ=env, roots=[root], persistent=lambda key: None)[0], off.resolve())
            if os.name == 'nt':
                self.assertEqual(espressif.resolve_idf(project, environ=env, roots=[root], persistent=lambda key: env.get(key))[0], idf.resolve())
            (tools / 'python_env/idf6.1_py3.14_env').rmdir()
            with self.assertRaisesRegex(ValueError, 'install'):
                espressif.resolve_idf(project, environ={}, roots=[root], persistent=lambda key: None, home=root)


if __name__ == '__main__':
    unittest.main()
