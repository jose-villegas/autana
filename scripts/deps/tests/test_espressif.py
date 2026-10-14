"""Isolated real-checkout and Windows registry selection tests."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'launcher/tools/build'))
import espressif
import json
import os
import subprocess
import tempfile
import unittest
from unittest import mock


class ResolutionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / 'project'
        (self.project / 'launcher').mkdir(parents=True)
        self.idf = self.checkout(self.root / 'esp/v6.1/esp-idf')
        self.commit = espressif.idf_commit(self.idf)
        self.pin = {'commit': self.commit, 'image': 'espressif/idf:release-v6.1@sha256:test'}
        (self.project / espressif.IDF_PIN).write_text(json.dumps(self.pin))
        self.tools = self.idf.parent / 'tools'
        (self.tools / 'python_env/idf6.1_py3.14_env').mkdir(parents=True)

    def checkout(self, path):
        (path / 'tools/cmake').mkdir(parents=True)
        (path / 'tools/cmake/version.cmake').write_text('set(IDF_VERSION_MAJOR 6)\nset(IDF_VERSION_MINOR 1)\n')
        git = ['git', '-c', 'user.name=test', '-c', 'user.email=test@example.org']
        subprocess.run([*git, 'init', '-q', str(path)], check=True)
        subprocess.run([*git, '-C', str(path), 'add', '.'], check=True)
        subprocess.run([*git, '-C', str(path), 'commit', '-qm', str(path)], check=True)
        return path

    def resolve(self, required=False, **kwargs):
        search = dict(environ={}, roots=[self.root / 'esp'], home=self.root,
                      persistent=lambda key: None)
        search.update(kwargs)
        return (espressif.require_idf if required else espressif.find_idf)(self.project, **search)

    def test_pinned_tools_root_ignores_the_environment_selection(self):
        other = str(self.root / 'other')
        search = dict(roots=[self.root / 'esp'], home=self.root, persistent=lambda key: None)
        with mock.patch.dict(os.environ, {'IDF_PATH': other, 'IDF_TOOLS_PATH': other}):
            self.assertEqual(espressif.pinned_tools_root(self.project, **search), self.tools.resolve())
        env = {'IDF_PATH': other, 'IDF_TOOLS_PATH': other}
        self.assertEqual(espressif.pinned_tools_root(self.project, environ=env, **search), self.tools.resolve())

    def test_commit_ignores_the_git_hook_environment(self):
        other = self.checkout(self.root / 'other-repo')
        hook = {'GIT_DIR': str(other / '.git'), 'GIT_WORK_TREE': str(other)}
        with mock.patch.dict(os.environ, hook):
            self.assertEqual(espressif.idf_commit(self.idf), self.commit)
            self.assertEqual(self.resolve(environ=dict(hook))[0], self.idf.resolve())

    def test_no_install_lookups_do_not_raise(self):
        checkout, tools = self.resolve(roots=[])
        self.assertEqual(checkout, self.root / 'esp/esp-idf')
        self.assertEqual(tools, self.root / '.espressif')
        with mock.patch.object(espressif, 'idf_project', return_value=self.project), \
             mock.patch.object(Path, 'home', return_value=self.root), \
             mock.patch.dict(os.environ, {}, clear=True), \
             mock.patch.object(espressif, 'persistent_environment', return_value=None):
            # Remove the only candidate from the default roots as well.
            self.idf.rename(self.root / 'hidden-idf')
            self.assertEqual(espressif.idf_path(), checkout)
            self.assertEqual(espressif.espressif_tools_root(), tools)
            sys.path.insert(0, str(espressif.PROJECT_ROOT / 'scripts/lib'))
            import pinned_tool
            list(pinned_tool.candidates('clang-format'))

    def test_no_install_requires_clone_commands(self):
        with self.assertRaisesRegex(ValueError, 'git clone') as failure:
            self.resolve(True, roots=[])
        self.assertIn(self.commit, str(failure.exception))
        self.assertIn('submodule update --init --recursive', str(failure.exception))

    def test_explicit_tools_lookup_skips_checkout(self):
        with mock.patch.dict(os.environ, {'IDF_TOOLS_PATH': str(self.tools)}), \
             mock.patch.object(espressif, 'find_idf', side_effect=AssertionError('checkout lookup')):
            self.assertEqual(espressif.espressif_tools_root(), self.tools)

    def test_tools_only_override(self):
        self.assertEqual(self.resolve(roots=[], environ={'IDF_TOOLS_PATH': str(self.tools)})[1], self.tools)

    def test_default_home_roots(self):
        self.assertEqual(self.resolve(roots=None)[0], self.idf)

    def test_parent_install_entry(self):
        (self.project / 'autana.local.toml').write_text(f"idf_installs = ['{self.idf.parent.as_posix()}']\n")
        self.assertEqual(self.resolve(roots=[])[0], self.idf)

    def test_checkout_install_entry(self):
        (self.project / 'autana.local.toml').write_text(f"idf_installs = ['{self.idf.as_posix()}']\n")
        self.assertEqual(self.resolve(roots=[])[0], self.idf)

    def test_off_pin_entry_is_skipped(self):
        off = self.checkout(self.root / 'off')
        (self.project / 'autana.local.toml').write_text(f"idf_installs = ['{off.as_posix()}', '{self.idf.as_posix()}']\n")
        self.assertEqual(self.resolve(roots=[])[0], self.idf)

    @unittest.skipUnless(os.name == 'nt', 'Windows installer defaults')
    def test_registry_off_pin_is_skipped(self):
        off = self.checkout(self.root / 'off')
        self.assertEqual(self.resolve(persistent=lambda key: str(off) if key == 'IDF_PATH' else None)[0], self.idf)

    @unittest.skipUnless(os.name == 'nt', 'Windows path normalization')
    def test_process_default_normalizes_case_and_slashes(self):
        off = self.checkout(self.root / 'off')
        self.assertEqual(self.resolve(environ={'IDF_PATH': str(off).upper().replace('\\', '/')},
                                     persistent=lambda key: str(off) if key == 'IDF_PATH' else None)[0], self.idf)

    def test_explicit_checkout_overrides_pin(self):
        off = self.checkout(self.root / 'off')
        self.assertEqual(self.resolve(environ={'IDF_PATH': str(off), 'IDF_TOOLS_PATH': str(self.tools)})[0], off)

    def test_detached_head(self):
        subprocess.run(['git', '-C', str(self.idf), 'checkout', '-q', '--detach'], check=True)
        self.assertEqual(self.resolve()[0], self.idf)

    def test_tools_pair_by_minor(self):
        (self.tools / 'python_env/idf6.1_py3.14_env').rmdir()
        (self.tools / 'python_env/idf5.5_py3.14_env').mkdir()
        self.assertEqual(self.resolve()[1], self.root / '.espressif')

    def test_home_tools_fallback(self):
        (self.tools / 'python_env/idf6.1_py3.14_env').rmdir()
        fallback = self.root / '.espressif'
        (fallback / 'python_env/idf6.1_py3.9_env').mkdir(parents=True)
        self.assertEqual(self.resolve(True)[1], fallback)

    def test_missing_tools_message_is_install_only(self):
        (self.tools / 'python_env/idf6.1_py3.14_env').rmdir()
        with self.assertRaisesRegex(ValueError, 'install') as failure:
            self.resolve(True)
        self.assertNotIn('git clone', str(failure.exception))
        self.assertIn(str(self.idf), str(failure.exception))

    def python_with_env(self, candidate):
        """idf_python() with the paired tools' 6.1 python present and
        IDF_PYTHON_ENV_PATH naming `candidate`'s environment; returns both."""
        good = self.tools / 'python_env/idf6.1_py3.14_env/Scripts/python.exe'
        good.parent.mkdir()
        good.touch()
        candidate.parent.mkdir(parents=True)
        candidate.touch()
        with mock.patch.object(espressif, 'find_idf', return_value=(self.idf, self.tools)),              mock.patch.object(espressif, 'idf_project', return_value=self.project),              mock.patch.dict(os.environ, {'IDF_PYTHON_ENV_PATH': str(candidate.parent.parent)}):
            return Path(espressif.idf_python()), good

    @unittest.skipUnless(os.name == 'nt', 'Windows interpreter pairing')
    def test_python_rejects_other_minor(self):
        chosen, good = self.python_with_env(self.tools / 'python_env/idf5.5_py3.14_env/Scripts/python.exe')
        self.assertEqual(chosen, good)

    @unittest.skipUnless(os.name == 'nt', 'Windows interpreter pairing')
    def test_python_ignores_right_minor_outside_paired_tools(self):
        chosen, good = self.python_with_env(self.root / 'elsewhere/python_env/idf6.1_py3.14_env/Scripts/python.exe')
        self.assertEqual(chosen, good)


@unittest.skipUnless(os.name == 'nt', 'real winreg reads')
class RegistryTests(unittest.TestCase):
    def setUp(self):
        import winreg
        import uuid
        self.winreg = winreg
        self.key = 'Software\\AutanaTests-' + uuid.uuid4().hex
        self.locations = [(winreg.HKEY_CURRENT_USER, self.key + '\\' + suffix) for suffix in ('User', 'Machine')]
        self.addCleanup(winreg.DeleteKey, winreg.HKEY_CURRENT_USER, self.key)
        for hive, name in self.locations:
            winreg.CreateKey(hive, name).Close()
            self.addCleanup(winreg.DeleteKey, hive, name)

    def put(self, index, value):
        hive, name = self.locations[index]
        with self.winreg.OpenKey(hive, name, 0, self.winreg.KEY_SET_VALUE) as key:
            self.winreg.SetValueEx(key, 'IDF_PATH', 0, self.winreg.REG_SZ, value)

    def test_user_precedes_machine(self):
        self.put(0, 'user')
        self.put(1, 'machine')
        self.assertEqual(espressif.persistent_environment('IDF_PATH', self.locations), 'user')

    def test_machine_fallback(self):
        self.put(1, 'machine')
        self.assertEqual(espressif.persistent_environment('IDF_PATH', self.locations), 'machine')

    def test_absent_returns_none(self):
        self.assertIsNone(espressif.persistent_environment('IDF_PATH', self.locations))


if __name__ == '__main__':
    unittest.main()
