"""apt_install.sh survives a package-mirror outage: retries, then the fallback mirror."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / 'scripts/ci/apt_install.sh'
RUNNER_SOURCES = ('Types: deb\nURIs: http://azure.archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates\n\n'
                  'Types: deb\nURIs: http://security.ubuntu.com/ubuntu/\nSuites: noble-security\n')


def setting(name):
    return re.search(rf'^{name}=(\S+)', SCRIPT.read_text(), re.M).group(1)


@unittest.skipUnless(shutil.which('sh') and os.name != 'nt', 'needs a POSIX shell')
class AptInstallTests(unittest.TestCase):
    def run_script(self, failing_updates):
        """Runs the script with an apt-get whose first `failing_updates` updates fail; returns its calls and sources."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = root / 'etc/apt/sources.list.d/ubuntu.sources'
            sources.parent.mkdir(parents=True)
            sources.write_text(RUNNER_SOURCES)
            stub = root / 'bin/apt-get'
            stub.parent.mkdir()
            stub.write_text('#!/bin/sh\necho "$*" >> "$CALLS"\ncase "$*" in *" update")\n'
                            '  n=$(grep -c " update$" "$CALLS"); [ "$n" -gt "$FAILING" ] || exit 100;;\nesac\n')
            stub.chmod(0o755)
            env = dict(os.environ, PATH=f'{stub.parent}{os.pathsep}{os.environ["PATH"]}', APT_DIR=str(root),
                       CALLS=str(root / 'calls'), FAILING=str(failing_updates))
            result = subprocess.run(['sh', str(SCRIPT), '-y', 'libportaudio2'], env=env, capture_output=True, text=True)
            calls = (root / 'calls').read_text().splitlines()
            return result, calls, sources.read_text()

    def test_every_call_retries_and_installs_with_fix_missing(self):
        result, calls, sources = self.run_script(0)
        self.assertEqual(result.returncode, 0, result.stderr)
        retries = f'-o Acquire::Retries={setting("APT_RETRIES")}'
        self.assertEqual(calls, [f'{retries} update', f'{retries} install --fix-missing -y libportaudio2'])
        self.assertEqual(sources, RUNNER_SOURCES)

    def test_a_failed_update_moves_the_runner_mirror_to_the_fallback_and_installs(self):
        result, calls, sources = self.run_script(1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([call.split()[-1] for call in calls], ['update', 'update', 'libportaudio2'])
        fallback = setting('FALLBACK_MIRROR')
        self.assertIn(f'URIs: {fallback}/\n', sources)
        self.assertNotIn('azure', sources)
        self.assertIn('http://security.ubuntu.com/ubuntu/', sources)

    def test_the_fallback_failing_too_fails_the_step(self):
        result, calls, _ = self.run_script(2)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('install', ' '.join(calls))


if __name__ == '__main__':
    unittest.main()
