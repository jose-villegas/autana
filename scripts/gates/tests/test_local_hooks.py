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
                                         'launcher/tools/render/generated_blocks.py',
                                         'launcher/tools/render/doc_tables.py')):
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

    def mark_host_tests(self):
        # CI's push-test step, as the push gate finds it.
        self.write('.github/workflows/host-tests.yml',
                   (ROOT / '.github/workflows/host-tests.yml').read_text())

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
        path = 'launcher/main/apps/sand/code_layout.txt'
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

    def test_a_repository_holding_the_gates_as_a_submodule_runs_its_own_marked_steps(self):
        consumer = self.root / 'consumer'
        consumer.mkdir()
        def git(*args):
            return subprocess.check_output(['git', '-c', 'protocol.file.allow=always', *args],
                                           cwd=consumer, text=True)
        git('init', '-q')
        git('submodule', '--quiet', 'add', self.root.as_posix(), 'third_party/engine')
        (consumer / '.github/workflows').mkdir(parents=True)
        (consumer / '.github/workflows/standards.yml').write_text(yaml.safe_dump({'jobs': {'gates': {'steps': [
            {'run': 'python3 third_party/engine/scripts/gates/check_file_headers.py',
             'env': {'STAGED_TEXT_GATE': 'true'}}]}}}))
        (consumer / 'src').mkdir()
        (consumer / 'src/frames.hpp').write_text('#pragma once\nclass Frames {};\n')
        # A folder of the consumer's own by that name does not displace the submodule's gates.
        (consumer / 'scripts/gates').mkdir(parents=True)
        (consumer / 'scripts/gates/notes.txt').write_text('the consumer\'s own\n')
        git('add', '.github', 'src', 'scripts')
        hook = consumer / 'third_party/engine/scripts/git-hooks/pre-commit'
        def commit_hook():
            result = subprocess.run([self.shell, hook.as_posix()], cwd=consumer,
                                    capture_output=True, text=True, timeout=120)
            return result.stdout + result.stderr
        self.assertIn('src/frames.hpp: no purpose', commit_hook())
        (consumer / 'src/frames.hpp').write_text('// The frames a capture holds.\n#pragma once\nclass Frames {};\n')
        git('add', 'src')
        self.assertNotIn('no purpose', commit_hook())

    def test_a_repository_with_no_gates_commits_and_pushes_unchecked(self):
        plain = self.root / 'plain'
        plain.mkdir()
        subprocess.check_call(['git', 'init', '-q'], cwd=plain)
        (plain / 'photo.png').write_bytes(b'\x89PNG')
        subprocess.check_call(['git', 'add', 'photo.png'], cwd=plain)
        for hook, stdin in (('pre-commit', ''), ('pre-push', 'refs/heads/x 1 refs/heads/x 0\n')):
            result = subprocess.run([self.shell, (self.root / 'scripts/git-hooks' / hook).as_posix()],
                                    cwd=plain, input=stdin, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('skipped', result.stderr)

    def test_header_pair_uses_staged_companion(self):
        self.write('launcher/main/gfx/sample.h', '/* Portable sample. */\n#pragma once\nint sample(void);\n')
        self.write('launcher/main/gfx/sample.c', '#include "gfx/sample.h"\n')
        result = self.gate('check-text-rules-staged.sh')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.write('launcher/main/gfx/sample.h', '#pragma once\nint sample(void);\n')
        result = self.gate('check-text-rules-staged.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no header comment', result.stdout + result.stderr)

    def test_deleting_the_only_module_header_is_checked(self):
        self.write('launcher/main/gfx/sample.h', '/* Portable sample. */\n#pragma once\nint sample(void);\n')
        self.write('launcher/main/gfx/sample.c', 'int sample;\n')
        self.git('-c', 'core.hooksPath=', 'commit', '-qm', 'module')
        self.git('rm', 'launcher/main/gfx/sample.h')
        result = self.gate('check-text-rules-staged.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no header comment', result.stdout + result.stderr)

    def test_a_snapshot_gate_judges_the_index_as_a_whole(self):
        # Fails when any snapshot file says "stale", and when handed --paths.
        self.write('scripts/gates/stale_gate.py',
                   '"""Fail when a catalogue file says stale."""\n'
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
        self.mark_host_tests()
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
        # A repository that marks no push tests pushes its source unchecked.
        (self.root / '.github/workflows/host-tests.yml').unlink()
        self.git('rm', '-q', '--cached', '.github/workflows/host-tests.yml')
        result = self.gate('check-host-tests-push.sh', update)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('SANITIZED', result.stdout)
        self.mark_host_tests()
        # A moved submodule pin is source the tests build.
        self.git('update-index', '--add', '--cacheinfo', f'160000,{base},third_party/engine')
        self.git('-c', 'core.hooksPath=', 'commit', '-qm', 'pin')
        pin = self.git('rev-parse', 'HEAD')
        result = self.gate('check-host-tests-push.sh', f'refs/heads/feature/test {pin} refs/heads/feature/test {head}\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count('SANITIZED'), 1)


    def test_a_new_branch_with_no_history_in_common_with_main_is_judged_by_its_own_files(self):
        self.write('note.txt', 'base\n')
        self.write('main.c', 'int main_unit;\n')
        self.git('add', 'main.c')
        self.git('-c', 'core.hooksPath=', 'commit', '-qm', 'base')
        # The remote's default branch, by whatever name it has.
        self.git('update-ref', 'refs/remotes/origin/trunk', self.git('rev-parse', 'HEAD'))
        self.git('symbolic-ref', 'refs/remotes/origin/HEAD', 'refs/remotes/origin/trunk')
        self.mark_host_tests()
        runner = self.root / 'launcher/test/run_tests.sh'
        runner.parent.mkdir(parents=True)
        runner.write_bytes(b'#!/bin/sh\necho SANITIZED\n')
        zero = '0' * 40

        self.git('checkout', '-q', '-b', 'notes')
        self.write('note.txt', 'edited\n')
        self.git('-c', 'core.hooksPath=', 'commit', '-qam', 'notes')
        notes = self.git('rev-parse', 'HEAD')
        result = self.gate('check-host-tests-push.sh', f'refs/heads/notes {notes} refs/heads/notes {zero}\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('SANITIZED', result.stdout)

        self.git('checkout', '-q', '--orphan', 'images')
        self.git('rm', '-rq', '--cached', '.')
        self.write('image.png', 'png\n')
        self.git('add', 'image.png')
        self.git('-c', 'core.hooksPath=', 'commit', '-qm', 'images')
        images = self.git('rev-parse', 'HEAD')
        result = self.gate('check-host-tests-push.sh', f'refs/heads/images {images} refs/heads/images {zero}\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('SANITIZED', result.stdout)

        self.write('sample.c', 'int sample;\n')
        self.git('add', 'sample.c')
        self.mark_host_tests()
        self.git('-c', 'core.hooksPath=', 'commit', '-qm', 'source')
        source = self.git('rev-parse', 'HEAD')
        result = self.gate('check-host-tests-push.sh', f'refs/heads/images {source} refs/heads/images {zero}\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('SANITIZED', result.stdout)


if __name__ == '__main__':
    unittest.main()
