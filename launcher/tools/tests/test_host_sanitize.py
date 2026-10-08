"""Sanitizer flags follow compiler link support, with Linux runtime flags intact."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts/device'))
import device


class HostSanitizeTests(unittest.TestCase):
    def flags(self, runtime):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            compiler = root / 'cc'
            compiler.write_bytes(('#!/bin/sh\ncase "$*" in\n'
                '    *fsanitize-recover*) exit ' + ('0' if runtime else '1') + ' ;;\n'
                '    *fsanitize-undefined-trap-on-error*) exit 0 ;;\n'
                '    *) exit 9 ;;\nesac\n').encode())
            compiler.chmod(0o755)
            script = root / 'probe.sh'
            script.write_bytes((f'. "{(ROOT / "launcher/tools/build/host_make.sh").as_posix()}"\n'
                                'uname() { echo Linux; }\n'
                                f'host_sanitizer_flags "{compiler.as_posix()}"\n').encode())
            shell = device.git_bash() if os.name == 'nt' else shutil.which('sh')
            return subprocess.run([shell, script.as_posix()], capture_output=True,
                                  text=True, timeout=30)

    def test_missing_runtime_uses_traps_even_on_linux(self):
        result = self.flags(False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(),
                         '-fsanitize=undefined -fsanitize-undefined-trap-on-error')

    def test_linux_runtime_keeps_recovery_and_address_sanitizer(self):
        result = self.flags(True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '-fsanitize=undefined '
                         '-fsanitize-recover=undefined -fsanitize=address -fno-omit-frame-pointer')


if __name__ == '__main__':
    unittest.main()
