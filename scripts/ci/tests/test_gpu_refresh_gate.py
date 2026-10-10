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
                                             ('hosted', 'false', 'true')]:
            with self.subTest(fingerprint=fingerprint, current=current), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                env = dict(os.environ, FINGERPRINT='hosted', GPU_ENV=str(root / 'env'),
                           GITHUB_ENV=str(root / 'github-env'), GITHUB_OUTPUT=str(root / 'output'))
                files = {
                    'launcher/tools/r3d/gpu_env.sh': 'export GPU_ENV\n',
                    'launcher/tools/render/render_doc_images.sh': f'echo {fingerprint}\n',
                    'env/bin/python3': f'#!/bin/sh\nprintf "%s\\n" "$*" > lookup\necho {current}\n'}
                for name, content in files.items():
                    path = root / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(content)
                    path.chmod(0o755)
                result = subprocess.run(['sh', str(ROOT / 'scripts/ci/gpu_refresh_gate.sh')],
                                        cwd=root, env=env, capture_output=True, text=True)
                output = root / 'output'
                if render is None:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('GPU inputs gpu differ from hosted inputs hosted', result.stderr)
                    self.assertFalse(output.exists())
                    self.assertFalse((root / 'lookup').exists())
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(output.read_text(), f'render={render}\n')
                    self.assertEqual((root / 'github-env').read_text(), f"GPU_ENV={env['GPU_ENV']}\n")
                    self.assertEqual((root / 'lookup').read_text(), 'scripts/ci/inputs_recorded.py hosted\n')
