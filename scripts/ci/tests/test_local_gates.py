"""Local pull-request gates stay mapped to the workflow source."""
import contextlib
import io
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest import mock

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import job_inputs
import local_gates

ROOT = Path(__file__).resolve().parents[3]


class LocalGatesTests(unittest.TestCase):
    def fixture(self, text):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        folder = root / '.github/workflows'
        folder.mkdir(parents=True)
        (folder / 'test.yml').write_text(text)
        import shutil
        shutil.copytree(ROOT / '.github/actions', root / '.github/actions')
        for args in [('init', '-q'),
                     ('-c', 'user.name=Test', '-c', 'user.email=test@example.com',
                      'commit', '--allow-empty', '-qm', 'fixture'),
                     ('update-ref', 'refs/remotes/origin/main', 'HEAD')]:
            subprocess.run(['git', '-C', str(root), '-c', 'commit.gpgsign=false',
                            '-c', 'core.hooksPath=/dev/null', *args], check=True, capture_output=True)
        return root

    def test_recursive_composite_defaults_and_job_tier(self):
        root = self.fixture("""on: [pull_request]
jobs:
  check:
    steps:
      - run: echo before
      - uses: ./.github/actions/outer
        with: {variant: diag}
""")
        for name, text in {
            'outer': """inputs:
  variant: {default: rel}
runs:
  using: composite
  steps:
    - uses: ./.github/actions/inner
      with: {command: 'autana build ${{ inputs.variant }}'}
""",
            'inner': """inputs:
  command: {default: echo default}
runs:
  using: composite
  steps:
    - shell: bash
      run: ${{ inputs.command }}
    - shell: bash
      run: python scripts/deps/idf_pin.py check
"""}.items():
            folder = root / '.github/actions' / name
            folder.mkdir(parents=True, exist_ok=True)
            (folder / 'action.yml').write_text(text)
        rows = list(local_gates.rows(root))
        self.assertEqual([row['tier'] for row in rows], ['diag'] * 3)
        self.assertEqual(rows[1]['command'], 'autana build diag')
        self.assertEqual(rows[2]['kind'], 'gate')

    def test_shell_templates(self):
        root = self.fixture('on: [pull_request]\njobs: {}')
        for shell, flags in [('', ['-e']), ('bash', ['--noprofile', '--norc', '-eo', 'pipefail']),
                             ('sh', ['-e'])]:
            script = root / 'step.sh'
            command = local_gates.shell_command({'shell': shell}, 'false | true', root, script)
            self.assertEqual(command[1:-1], flags)
            self.assertEqual(command[-1], script.as_posix())
            self.assertEqual(script.read_text(), 'false | true')

    def test_mapping_without_base_ref_and_run_requires_remote_branch(self):
        root = self.fixture("""on: [pull_request]
jobs:
  check:
    steps:
      - run: echo ${{ github.base_ref }}
""")
        local_gates.git(root, 'update-ref', '-d', 'refs/remotes/origin/main')
        self.assertEqual(local_gates.git(root, 'remote').strip(), b'')
        self.assertEqual([row['command'] for row in local_gates.rows(root)], ['echo main'])
        capture = io.StringIO()
        with contextlib.redirect_stdout(capture):
            self.assertEqual(local_gates.main(['list', '--root', str(root)]), 0)
        self.assertIn('echo main', capture.getvalue())
        sha = local_gates.git(root, 'rev-parse', 'HEAD').decode().strip()
        for base in (sha, 'origin/missing', 'refs/remotes/origin/missing', 'HEAD'):
            with self.subTest(base=base):
                with self.assertRaisesRegex(ValueError, '--base takes a branch such as origin/main'):
                    local_gates.run(root, root / 'logs', base=base)
                errors = io.StringIO()
                with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as stopped:
                    local_gates.main(['run', '--root', str(root), '--out', str(root / 'logs'),
                                      '--base', base])
                self.assertEqual(stopped.exception.code, 2)
                self.assertIn('--base takes a branch such as origin/main', errors.getvalue())

    def test_diag_tier_does_not_run_host_steps(self):
        root = self.fixture("""on: [pull_request]
jobs:
  host:
    steps:
      - name: Host gate
        run: echo host-gate
      - name: Another host gate
        run: echo another-host-gate
  diag:
    steps:
      - uses: ./.github/actions/idf-build
        with:
          command: ../tools/autana build diag --project ..
""")
        capture = io.StringIO()
        with mock.patch.object(local_gates, 'affected_jobs', return_value={}), \
                mock.patch.object(local_gates, 'shell_command',
                                  return_value=[sys.executable, '-c', 'pass']) as commands, \
                contextlib.redirect_stdout(capture):
            self.assertEqual(local_gates.run(root, root / 'logs', tiers=('diag',)), 0)
        self.assertTrue(commands.call_args_list)
        self.assertTrue(all(call.args[0]['tier'] == 'diag'
                            for call in commands.call_args_list))
        for label in ('Host gate', 'Another host gate'):
            self.assertIn(f'NOT RUN host: test:host: {label}', capture.getvalue())

    def test_run_continue_on_error(self):
        for setting, expected in [('continue-on-error: true', 0), ('', 1)]:
            with self.subTest(setting=setting):
                root = self.fixture(f"""on: [pull_request]
jobs:
  check:
    steps:
      - id: probe
        {setting}
        run: exit 1
      - if: success() && steps.probe.outcome == 'failure'
        run: echo continued > continued.txt
  dependent:
    needs: check
    if: needs.check.result == 'success'
    steps:
      - run: echo dependent > dependent.txt
""")
                capture = io.StringIO()
                with mock.patch.object(local_gates, 'affected_jobs', return_value={}), \
                        contextlib.redirect_stdout(capture):
                    self.assertEqual(local_gates.run(root, root / 'logs'), expected)
                self.assertEqual((root / 'continued.txt').exists(), expected == 0)
                self.assertEqual((root / 'dependent.txt').exists(), expected == 0)
                verdict = 'ok' if expected == 0 else 'FAIL'
                self.assertIn(f'{verdict} {root / "logs/000-test-check.log"}:',
                              capture.getvalue())
                if expected == 0:
                    self.assertIn('failed; CI continues', capture.getvalue())
                    self.assertIn('3 ok, 0 failed', capture.getvalue())
                self.assertIs(list(local_gates.rows(root))[0]['continue_on_error'],
                              expected == 0)

    def test_github_path_reaches_next_step(self):
        root = self.fixture("""on: [pull_request]
jobs:
  check:
    steps:
      - run: |
          mkdir -p "$RUNNER_TEMP/bin"
          printf '#!/bin/sh\\nexit 0\\n' > "$RUNNER_TEMP/bin/local-path-probe"
          chmod +x "$RUNNER_TEMP/bin/local-path-probe"
          echo "$RUNNER_TEMP/bin" >> "$GITHUB_PATH"
      - run: local-path-probe
""")
        with mock.patch.object(local_gates, 'affected_jobs', return_value={}):
            self.assertEqual(local_gates.run(root, root / 'logs'), 0)

    def test_working_tree_inputs_and_credentials(self):
        root = self.fixture("""on: [pull_request]
jobs:
  inputs:
    steps:
      - run: python scripts/ci/job_inputs.py affected --job check
  check:
    steps:
      - run: echo gate
""")
        import os
        import subprocess
        for args in [('init', '-q'), ('add', '.'),
                     ('-c', 'user.name=Test', '-c', 'user.email=test@example.com', 'commit', '-qm', 'fixture'),
                     ('remote', 'add', 'origin', 'https://github.com/example/test.git'),
                     ('update-ref', 'refs/remotes/origin/main', 'HEAD')]:
            subprocess.run(['git', '-C', str(root), '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null', *args], check=True, capture_output=True)
        workflow = root / '.github/workflows/test.yml'
        workflow.write_text(workflow.read_text() + '# uncommitted\n')
        (root / 'new.txt').write_text('untracked')

        def affected(workflow, jobs, changes, base, files, **kwargs):
            self.assertIn({'filename': '.github/workflows/test.yml', 'status': 'modified'}, changes)
            self.assertIn({'filename': 'new.txt', 'status': 'added'}, changes)
            kwargs['reporter']('check', 'true', 'matches: input')
        with mock.patch.dict(os.environ, {'GH_TOKEN': 'local-token'}):
            before = dict(os.environ)
            with mock.patch.object(local_gates, 'affected_changes', side_effect=affected) as request:
                self.assertEqual(local_gates.affected_jobs(root, 'origin/main')['test:check'], 'affected: input')
                request.assert_called_once()
            self.assertEqual(dict(os.environ), before)

    def test_real_workflows_are_mapped(self):
        rows = list(local_gates.rows(ROOT))
        self.assertEqual([row for row in rows if row['kind'] == 'warn'], [])
        for job in local_gates.pr_jobs(ROOT):
            self.assertTrue(any(row['job'] == job['key'] and row['kind'] in {'gate', 'skip'}
                                for row in rows), job['key'])
        commands = '\n'.join(row['command'] + str(row.get('env', {})) for row in rows if row['kind'] == 'gate')
        for command in ('build_diag_check.sh --gates-only',
                        'check_comment_length.py', 'run_tests.sh --sanitize'):
            self.assertIn(command, commands)

    def test_real_exclusion_rules_have_owners(self):
        jobs = [job for workflow in local_gates.workflows(ROOT)
                for job in workflow['jobs'].values()]
        for reason, predicate in local_gates.OUT_OF_SCOPE:
            self.assertTrue(any(predicate(job) or any(predicate(step)
                for step in job.get('steps', [])) for job in jobs), reason)

    def test_exclusions_and_unknown_action(self):
        examples = [
            ('runs-on: [self-hosted, linux]', 'board / GPU'),
            ('env: {KEY: "${{ secrets.PRIVATE }}"}', 'secret'),
            ('uses: org/repo/.github/workflows/check.yml@main', "another branch's workflow"),
            ('if: github.event.pull_request.body', 'reads the pull request'),
            ('outputs: {result: x}', 'CI wiring'),
        ]
        for extra, reason in examples:
            root = self.fixture('on: [pull_request]\njobs:\n  check:\n    ' + extra +
                                '\n    steps: []\n')
            self.assertIn(reason, [row['reason'] for row in local_gates.rows(root)])
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - uses: unknown/action@main
      - run: echo '${{ mystery.value }}'
      - if: github.event.pull_request.body
        run: echo private
''')
        rows = list(local_gates.rows(root))
        self.assertEqual([row['kind'] for row in rows], ['warn', 'warn', 'skip'])

    def test_setup_split_and_directory_tracking(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - run: |
          pip install package
          npm ci --prefix scripts/gates
          python3 check.py
      - uses: ./.github/actions/idf-build
        with:
          command: ../tools/autana build diag --project .. && cd .. && python3 gate.py
''')
        rows = list(local_gates.rows(root))
        gates = [row for row in rows if row['kind'] == 'gate']
        self.assertGreaterEqual(len(gates), 3)
        self.assertEqual(rows[0]['tier'], 'diag')
        self.assertEqual(rows[0]['dropped'], ['pip install package'])
        self.assertIn(':\n', rows[0]['command'])
        self.assertTrue(all(row['tier'] == 'diag' for row in rows))
        build = next(row for row in rows if row['label'] == 'Build in the pinned IDF image')
        self.assertIn('../tools/autana build diag --project ..', build['env']['BUILD_COMMAND'])

    def test_expansion(self):
        ctx = {'github.base_ref': 'main', 'github.head_ref': 'feature/example',
               'github.sha': 'head'}
        self.assertEqual(local_gates.expand('${{ github.base_ref }} ${{ github.head_ref }} '
            '${{ github.sha }} ${{ unknown }}', ctx), 'main feature/example head ${{ unknown }}')

    def test_multiple_build_tiers(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - uses: ./.github/actions/idf-build
        with:
          command: ../tools/autana build rel --project .. && python3 release.py
      - uses: ./.github/actions/idf-build
        with:
          command: ../tools/autana build dev --project .. && python3 development.py
''')
        self.assertTrue(all(row['tier'] == 'rel,dev' for row in local_gates.rows(root)))

    def test_run_cd_keeps_its_gate(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - run: cd scripts && python3 check.py
''')
        rows = list(local_gates.rows(root))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['command'], 'cd scripts && python3 check.py')

    def test_non_pr_step_does_not_warn(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - if: github.event_name != 'pull_request'
        run: echo '${{ github.event.inputs.value }}'
      - run: echo gate
''')
        rows = list(local_gates.rows(root))
        self.assertEqual([row['kind'] for row in rows], ['skip', 'gate'])

    def test_unknown_condition_warns(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    if: unknown.value
    steps:
      - run: echo gate
''')
        self.assertTrue(any(row['kind'] == 'warn' for row in local_gates.rows(root)))

    def test_install_preparation_is_not_a_gate(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - run: |
          version=example
          sudo apt-get update
          curl https://example.invalid/$version | tar -x -C "$RUNNER_TEMP"
          echo "$RUNNER_TEMP/$version" >> "$GITHUB_PATH"
''')
        self.assertEqual([row['kind'] for row in local_gates.rows(root)], ['gate'])

    def test_inline_host_change_preserves_other_segments(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - run: pip install missing && python3 check.py
''')
        rows = list(local_gates.rows(root))
        self.assertEqual([row['kind'] for row in rows], ['gate'])
        self.assertEqual(rows[0]['command'], ': && python3 check.py')
        self.assertEqual(rows[0]['dropped'], ['pip install missing'])

    def test_failed_gate_prints_install_hint(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - name: Python requirements
        run: pip install -r requirements.txt
      - run: echo "ModuleNotFoundError No module named missing" >&2; exit 1
''')
        capture = io.StringIO()
        with mock.patch.object(local_gates, 'affected_jobs', return_value={}), \
                contextlib.redirect_stdout(capture):
            self.assertEqual(local_gates.run(root, root / 'logs', base='origin/main'), 1)
        self.assertIn('Host-wide change skipped (Python requirements: pip install -r requirements.txt)',
                      capture.getvalue())
        self.assertEqual(len(capture.getvalue().splitlines()), 4)

    def test_script_keeps_shell_state(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - name: Stateful script
        run: |
          set -eu
          value=kept
          pip install missing
          mkdir child
          cd child
          test "$value" = kept
          test "$PWD" = "$(cd "$GITHUB_WORKSPACE" && pwd)/child"
          echo state-kept
          false
          echo lost-errexit
''')
        capture = io.StringIO()
        with mock.patch.object(local_gates, 'affected_jobs', return_value={}), \
                contextlib.redirect_stdout(capture), mock.patch('builtins.print', wraps=print) as printed:
            self.assertEqual(local_gates.run(root, root / 'logs', base='origin/main'), 1)
        self.assertTrue(all(call.kwargs.get('flush') for call in printed.call_args_list))
        logs = list((root / 'logs').glob('*.log'))
        self.assertEqual(len(logs), 1)
        self.assertIn('state-kept', logs[0].read_text(encoding='utf-8'))
        self.assertNotIn('lost-errexit', logs[0].read_text(encoding='utf-8'))
        self.assertIn('dropped: pip install missing', capture.getvalue())
        self.assertIn('0 ok, 1 failed, 0 skipped, 0 not run', capture.getvalue())

    def test_checkout_setup_and_host_change(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - run: |
          git config --global example.key value
          npm ci --prefix scripts/gates
          git submodule update --init
      - run: sudo apt-get update
''')
        rows = list(local_gates.rows(root))
        self.assertEqual([row['kind'] for row in rows], ['setup', 'skip'])
        self.assertEqual(rows[-1]['reason'], 'host-wide change')

    def test_name_status_matches_api(self):
        fixtures = [('M\0code/a.c\0', {'filename': 'code/a.c', 'status': 'modified'}),
                    ('A\0code/new.c\0', {'filename': 'code/new.c', 'status': 'added'}),
                    ('R100\0code/a.c\0code/b.c\0', {'filename': 'code/b.c',
                      'previous_filename': 'code/a.c', 'status': 'renamed'}),
                    ('D\0code/a.c\0', {'filename': 'code/a.c', 'status': 'removed'})]
        manifest = {'files': ['code/a.c'], 'directories': ['code']}
        for raw, api in fixtures:
            with self.subTest(raw=raw):
                self.assertEqual(job_inputs.changes_from_name_status(raw), [api])
                self.assertEqual(job_inputs.matching_paths([api], manifest, ['code/a.c']),
                    job_inputs.matching_paths(job_inputs.changes_from_name_status(raw), manifest,
                                             ['code/a.c']))

    def test_run_environment_and_outputs(self):
        root = self.fixture('''on: [pull_request]
env:
  VALUE: workflow
jobs:
  smoke:
    runs-on: ubuntu-latest
    env:
      VALUE: job
    steps:
      - id: first
        run: |
          test -d "$RUNNER_TEMP"
          test "$(cd "$GITHUB_WORKSPACE" && pwd)" = "$PWD"
          echo result=ok >> "$GITHUB_OUTPUT"
      - env:
          VALUE: '${{ steps.first.outputs.result }}'
          TEMP_VALUE: '${{ runner.temp }}'
        run: test "$VALUE" = ok && test "$TEMP_VALUE" = "$RUNNER_TEMP"
''')
        with mock.patch.object(local_gates, 'affected_jobs', return_value={}):
            self.assertEqual(local_gates.run(root, root / 'logs', base='origin/main'), 0)


if __name__ == '__main__':
    unittest.main()
