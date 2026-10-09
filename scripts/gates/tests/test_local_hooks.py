"""Local hooks judge index content and the refs supplied by git."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tracked import tracked_files
from gate_tree import ShellGateTestCase


class LocalHookTests(ShellGateTestCase):
    def setUp(self):
        super().setUp()
        # Tracked files only: CI installs thousands of untracked npm files under
        # scripts/, which the fixture would otherwise copy and stage.
        for path in tracked_files(ROOT, ('scripts', 'launcher/tools/build',
                                         'launcher/tools/render/generated_blocks.py')):
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, self.root / path)
        (self.root / '.github/workflows').mkdir(parents=True)
        # CI's file-scoped gates as they are; its whole-tree ones need the
        # real documents, so test_a_snapshot_gate_judges_the_index_as_a_whole
        # declares its own.
        workflow = yaml.safe_load((ROOT / '.github/workflows/comment-rules.yml').read_text())
        for job in workflow['jobs'].values():
            job['steps'] = [step for step in job['steps']
                            if step.get('env', {}).get('STAGED_TEXT_GATE') != 'snapshot']
        self.workflow = workflow
        self.save_workflow()
        (self.root / 'launcher/main/apps/example').mkdir(parents=True)
        self.git('init', '-q')
        self.git('config', 'user.name', 'Test')
        self.git('config', 'user.email', 'test@example.invalid')
        # No background gc or maintenance may still be writing when the
        # temporary tree is removed.
        self.git('config', 'gc.auto', '0')
        self.git('config', 'maintenance.auto', 'false')
        self.git('add', '.github/workflows/comment-rules.yml')
        self.write('launcher/main/apps/example/development_only.cmake', '')
        self.git('add', 'scripts', 'launcher/tools')
        self.git('-c', 'core.hooksPath=', 'commit', '-qm', 'fixture tools')

    def save_workflow(self):
        (self.root / '.github/workflows/comment-rules.yml').write_text(yaml.safe_dump(self.workflow))

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root, text=True).strip()

    def write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
        self.git('add', path)

    def gate(self, name, stdin=None):
        result = subprocess.run([self.shell, (self.root / 'scripts/gates' / name).as_posix()],
                              cwd=self.root, input=stdin.encode() if stdin else None,
                              capture_output=True, timeout=60)
        result.stdout = result.stdout.decode()
        result.stderr = result.stderr.decode()
        return result

    def test_personal_path_is_read_from_index(self):
        path = 'launcher/main/render/code_layout.txt'
        bad = '/Users/' + 'someone/scratch\n'
        self.write(path, bad)
        (self.root / path).write_text('portable\n')
        result = self.gate('check-text-rules-staged.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('PERSONAL-PATH', result.stdout + result.stderr)
        self.assertIn('git add', result.stdout + result.stderr)
        self.git('add', path)
        (self.root / path).write_text(bad)
        result = self.gate('check-text-rules-staged.sh')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_header_pair_uses_staged_companion(self):
        self.write('launcher/main/gfx/sample.h', '/* Portable sample. */\n#pragma once\n')
        self.write('launcher/main/gfx/sample.c', '#include "gfx/sample.h"\n')
        result = self.gate('check-text-rules-staged.sh')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.write('launcher/main/gfx/sample.h', '#pragma once\n')
        result = self.gate('check-text-rules-staged.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no header comment', result.stdout + result.stderr)

    def test_deleting_the_only_module_header_is_checked(self):
        self.write('launcher/main/gfx/sample.h', '/* Portable sample. */\n#pragma once\n')
        self.write('launcher/main/gfx/sample.c', 'int sample;\n')
        self.git('-c', 'core.hooksPath=', 'commit', '-qm', 'module')
        self.git('rm', 'launcher/main/gfx/sample.h')
        result = self.gate('check-text-rules-staged.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no header comment', result.stdout + result.stderr)

    def test_a_snapshot_gate_judges_the_index_as_a_whole(self):
        # Fails when any snapshot file says "stale", and when handed --paths.
        self.write('scripts/gates/stale_gate.py',
                   'import pathlib, sys\n'
                   'assert "--paths" not in sys.argv, "a snapshot gate gets no paths"\n'
                   'root = pathlib.Path(__file__).resolve().parents[2]\n'
                   'found = [p for p in root.rglob("*.txt") if "stale" in p.read_text()]\n'
                   'print("stale:", *found) if found else print("current")\n'
                   'sys.exit(1 if found else 0)\n')
        steps = next(iter(self.workflow['jobs'].values()))['steps']
        steps.append({'name': 'Catalogue', 'env': {'STAGED_TEXT_GATE': 'snapshot'},
                      'run': 'python3 scripts/gates/stale_gate.py'})
        self.save_workflow()
        self.git('add', '.github/workflows/comment-rules.yml')
        path = 'docs/catalogue.txt'
        self.write(path, 'stale\n')
        (self.root / path).write_text('current\n')
        result = self.gate('check-text-rules-staged.sh')
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('stale:', result.stdout + result.stderr)
        self.assertIn('regenerate', result.stdout + result.stderr)
        self.git('add', path)
        (self.root / path).write_text('stale\n')
        result = self.gate('check-text-rules-staged.sh')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('current', result.stdout)

    def test_push_c_range_runs_sanitized_tests_and_refuses_failure(self):
        self.write('note.txt', 'base\n')
        self.git('-c', 'core.hooksPath=', 'commit', '-qm', 'base')
        base = self.git('rev-parse', 'HEAD')
        self.write('sample.c', 'int sample;\n')
        self.git('-c', 'core.hooksPath=', 'commit', '-qm', 'source')
        head = self.git('rev-parse', 'HEAD')
        runner = self.root / 'launcher/test/run_tests.sh'
        runner.parent.mkdir(parents=True)
        runner.write_bytes(b'#!/bin/sh\n[ "$1" = --sanitize ] || exit 9\necho SANITIZED\nexit 7\n')
        update = f'refs/heads/feature/test {head} refs/heads/feature/test {base}\n'
        result = self.gate('check-host-tests-push.sh', update)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SANITIZED', result.stdout, result.stderr)
        self.assertIn('--no-verify', result.stderr)
        result = self.gate('../git-hooks/pre-push', update)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SANITIZED', result.stdout, result.stderr)
        runner.write_bytes(b'#!/bin/sh\n[ "$1" = --sanitize ] || exit 9\necho SANITIZED\n')
        result = self.gate('check-host-tests-push.sh', update)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count('SANITIZED'), 1)
        result = self.gate('../git-hooks/pre-push', update + update)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count('SANITIZED'), 1)
        result = self.gate('check-host-tests-push.sh',
                           f'refs/heads/feature/test {base} refs/heads/feature/test {base}\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('SANITIZED', result.stdout)


if __name__ == '__main__':
    unittest.main()
