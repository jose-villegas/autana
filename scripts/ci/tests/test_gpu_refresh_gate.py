"""The GPU concurrency gate compares inputs before emitting its render decision."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class GpuRefreshGateTests(unittest.TestCase):
    def test_mismatch_current_and_changed_inputs(self):
        for fingerprint, current, render in [('gpu', 'true', None), ('hosted', 'true', 'false'),
                                             ('hosted', 'false', 'true'), ('hosted', 'error', None),
                                             ('hosted', 'obsolete: main moved to X', 'false')]:
            with self.subTest(fingerprint=fingerprint, current=current), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                env = dict(os.environ, OVERRIDE='true', FINGERPRINT='hosted', GPU_ENV=str(root / 'env'),
                           GITHUB_ENV=str(root / 'github-env'), GITHUB_OUTPUT=str(root / 'output'))
                lookup = 'exit 7' if current == 'error' else 'echo ' + current
                files = {
                    'scripts/lib/local_schedule.py': (ROOT / 'scripts/lib/local_schedule.py').read_text(),
                    'launcher/tools/r3d/gpu_env.sh': f'export GPU_ENV\nexport SOURCED_INPUTS={fingerprint}\n',
                    'launcher/tools/render/render_doc_images.sh': 'echo "$SOURCED_INPUTS"\n',
                    'env/bin/python3': f'#!/bin/sh\nprintf "%s\\n" "$*" > lookup\n{lookup}\n'}
                for name, content in files.items():
                    path = root / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(content)
                    path.chmod(0o755)
                gate = root / 'gate.sh'
                script = (ROOT / 'scripts/ci/gpu_refresh_gate.sh').read_text()
                gate.write_text(script.replace('set -eu', 'set -u') if current == 'error' else script)
                result = subprocess.run(['sh', str(gate)],
                                        cwd=root, env=env, capture_output=True, text=True)
                output = root / 'output'
                if render is None:
                    self.assertNotEqual(result.returncode, 0)
                    if current != 'error':
                        self.assertIn('GPU inputs gpu differ from hosted inputs hosted', result.stderr)
                    self.assertFalse(output.exists())
                    self.assertEqual((root / 'lookup').exists(), current == 'error')
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(output.read_text(), f'render={render}\n')
                    self.assertEqual((root / 'github-env').read_text(), f"GPU_ENV={env['GPU_ENV']}\n")
                    self.assertEqual((root / 'lookup').read_text(), 'scripts/ci/inputs_recorded.py hosted --skip-obsolete\n')
