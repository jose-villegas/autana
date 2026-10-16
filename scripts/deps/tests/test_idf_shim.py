"""The Windows cmd boundary with an installed ESP-IDF environment."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'launcher/tools/build'))
import espressif


@unittest.skipUnless(os.name == 'nt', 'Windows cmd shim')
class WindowsShimTests(unittest.TestCase):
    def test_broken_python_first_on_path(self):
        try:
            checkout, tools = espressif.require_idf()
        except ValueError as error:
            self.skipTest(str(error))
        python = Path(espressif.idf_python())
        self.assertTrue(python.is_relative_to(tools), python)
        query = subprocess.run([sys.executable, espressif.__file__, 'env'],
                               capture_output=True, text=True, timeout=30)
        self.assertEqual(query.returncode, 0, query.stderr)
        selected = dict(line.split('=', 1) for line in shlex.split(query.stdout))
        self.assertEqual(Path(selected['IDF_PYTHON']), python)
        shim = espressif.PROJECT_ROOT / 'launcher/tools/build/idf_shim.bat'
        with tempfile.TemporaryDirectory(prefix='idf shim ') as directory:
            root = Path(directory)
            broken_dir = root / 'broken python'
            broken_dir.mkdir()
            (broken_dir / 'python.bat').write_bytes(
                b'@echo ESP-IDF supports Python 3.10 or newer but you are using Python 3.9.13\r\n@exit /b 1\r\n')
            env = dict(os.environ, **selected)
            env.update(PATH=str(broken_dir) + os.pathsep + os.environ['PATH'],
                       IDF_PATH=str(checkout), IDF_TOOLS_PATH=str(tools),
                       IDF_SHIM_DIR=str(root), IDF_SHIM_EXPORT=str(checkout / 'export.bat'),
                       IDF_SHIM_PYTHON_DIR=str(Path(selected['IDF_PYTHON']).parent))
            cmd = os.environ['COMSPEC']
            broken = subprocess.run([cmd, '/d', '/c', 'python', '--version'],
                                    cwd=root, env=env, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(broken.returncode, 0)
            self.assertIn('Python 3.9.13', broken.stdout)
            result = subprocess.run(
                f'"{cmd}" /d /s /c ""{shim}" idf.py --version"',
                cwd=root, env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('ESP-IDF v', result.stdout)

    def test_export_without_idf_reports_environment_failure(self):
        shim = espressif.PROJECT_ROOT / 'launcher/tools/build/idf_shim.bat'
        with tempfile.TemporaryDirectory(prefix='idf export ') as directory:
            root = Path(directory)
            export = root / 'export.bat'
            export.write_bytes(b'@exit /b 0\r\n')
            env = dict(os.environ, PATH=str(Path(os.environ['SystemRoot']) / 'System32'),
                       IDF_SHIM_DIR=str(root), IDF_SHIM_EXPORT=str(export),
                       IDF_SHIM_PYTHON_DIR=str(Path(sys.executable).parent))
            result = subprocess.run(
                f'"{os.environ["COMSPEC"]}" /d /s /c ""{shim}" idf.py --version"',
                cwd=root, env=env, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('ESP-IDF environment did not load', result.stderr)
            self.assertNotIn('is not recognized', result.stderr)

    def test_missing_python_directory_reports_environment_failure(self):
        shim = espressif.PROJECT_ROOT / 'launcher/tools/build/idf_shim.bat'
        with tempfile.TemporaryDirectory(prefix='idf missing python ') as directory:
            root = Path(directory)
            export = root / 'export.bat'
            export.write_bytes(f'@set "PATH={root};%PATH%"\r\n'.encode())
            (root / 'idf.py.bat').write_bytes(b'@exit /b 0\r\n')
            env = dict(os.environ, IDF_SHIM_DIR=str(root), IDF_SHIM_EXPORT=str(export))
            env.pop('IDF_SHIM_PYTHON_DIR', None)
            result = subprocess.run(
                f'"{os.environ["COMSPEC"]}" /d /s /c ""{shim}" idf.py --version"',
                cwd=root, env=env, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('ESP-IDF environment did not load', result.stderr)
