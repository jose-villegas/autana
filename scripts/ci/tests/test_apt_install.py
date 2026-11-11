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

# An apt-get that logs each call to $CALLS and fails the first $FAIL_UPDATES updates and $FAIL_INSTALLS installs.
STUB = '''#!/bin/sh
echo "$*" >> "$CALLS"
for verb in update install; do
    case "$*" in *" $verb"*)
        n=$(grep -c " $verb" "$CALLS")
        if [ $verb = update ]; then limit=$FAIL_UPDATES; else limit=$FAIL_INSTALLS; fi
        [ "$n" -gt "$limit" ] || exit 100;;
    esac
done
'''


def setting(name):
    return re.search(rf'^{name}=(\S+)', SCRIPT.read_text(), re.M).group(1)


@unittest.skipUnless(shutil.which('sh') and os.name != 'nt', 'needs a POSIX shell')
class AptInstallTests(unittest.TestCase):
    def run_script(self, failing_updates=0, failing_installs=0):
        """Runs the script with an apt-get whose first updates and installs fail as asked; returns its calls and sources."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = root / 'etc/apt/sources.list.d/ubuntu.sources'
            sources.parent.mkdir(parents=True)
            sources.write_text(RUNNER_SOURCES)
            stub = root / 'bin/apt-get'
            stub.parent.mkdir()
            stub.write_text(STUB)
            stub.chmod(0o755)
            env = dict(os.environ, PATH=f'{stub.parent}{os.pathsep}{os.environ["PATH"]}', APT_DIR=str(root),
                       CALLS=str(root / 'calls'), FAIL_UPDATES=str(failing_updates),
                       FAIL_INSTALLS=str(failing_installs))
            result = subprocess.run(['sh', str(SCRIPT), '-y', 'libportaudio2'], env=env, capture_output=True, text=True)
            calls = (root / 'calls').read_text().splitlines()
            return result, calls, sources.read_text()

    def verbs(self, calls):
        return [call.split()[2] for call in calls]

    def assert_fell_back(self, sources):
        self.assertIn(f'URIs: {setting("FALLBACK_MIRROR")}/\n', sources)
        self.assertNotIn('azure', sources)
        self.assertIn('http://security.ubuntu.com/ubuntu/', sources)

    def test_every_call_retries_and_installs_with_fix_missing(self):
        result, calls, sources = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        retries = f'-o Acquire::Retries={setting("APT_RETRIES")}'
        self.assertEqual(calls, [f'{retries} update', f'{retries} install --fix-missing -y libportaudio2'])
        self.assertEqual(sources, RUNNER_SOURCES)

    def test_a_failed_update_moves_the_runner_mirror_to_the_fallback_and_installs(self):
        result, calls, sources = self.run_script(failing_updates=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.verbs(calls), ['update', 'update', 'install'])
        self.assert_fell_back(sources)

    def test_a_failed_download_after_a_good_update_falls_back_too(self):
        result, calls, sources = self.run_script(failing_installs=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.verbs(calls), ['update', 'install', 'update', 'install'])
        self.assert_fell_back(sources)

    def test_the_fallback_failing_too_fails_the_step(self):
        result, calls, _ = self.run_script(failing_installs=2)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.verbs(calls), ['update', 'install', 'update', 'install'])

if __name__ == '__main__':
    unittest.main()
