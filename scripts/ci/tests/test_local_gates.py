"""Local pull-request gates stay mapped to the workflow source."""
import contextlib
import io
from pathlib import Path
import sys
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
        return root

    def test_real_workflows_are_mapped(self):
        rows = list(local_gates.rows(ROOT))
        self.assertEqual([row for row in rows if row['kind'] == 'warn'], [])
        for job in local_gates.pr_jobs(ROOT):
            self.assertTrue(any(row['job'] == job['key'] and row['kind'] in {'gate', 'skip'}
                                for row in rows), job['key'])
        commands = '\n'.join(row['command'] for row in rows if row['kind'] == 'gate')
        for command in ('check_no_soft_double.py', 'misra_tidy_gate.py',
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
        self.assertEqual([row['kind'] for row in rows], ['skip', 'setup', 'gate', 'gate', 'gate'])
        self.assertEqual(rows[0]['tier'], 'host')
        self.assertEqual(rows[-1]['dir'], '.')
        self.assertEqual(rows[-1]['tier'], 'diag')
        self.assertIn('--diag', rows[-2]['command'])

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
        self.assertEqual([row['tier'] for row in local_gates.rows(root)],
                         ['rel', 'rel', 'dev', 'dev'])

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
        self.assertEqual([row['kind'] for row in local_gates.rows(root)], ['skip'])

    def test_inline_install_preserves_gate(self):
        root = self.fixture('''on: [pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - run: pip install missing && python3 check.py
''')
        rows = list(local_gates.rows(root))
        self.assertEqual([row['kind'] for row in rows], ['skip', 'gate'])
        self.assertEqual(rows[-1]['command'], 'python3 check.py')

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
            self.assertEqual(local_gates.run(root, root / 'logs', base='main'), 1)
        self.assertIn('Install skipped (Python requirements): pip install -r requirements.txt',
                      capture.getvalue())

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
          test "$GITHUB_WORKSPACE" = "$PWD"
          echo result=ok >> "$GITHUB_OUTPUT"
      - env:
          VALUE: '${{ steps.first.outputs.result }}'
          TEMP_VALUE: '${{ runner.temp }}'
        run: test "$VALUE" = ok && test "$TEMP_VALUE" = "$RUNNER_TEMP"
''')
        with mock.patch.object(local_gates, 'affected_jobs', return_value={}):
            self.assertEqual(local_gates.run(root, root / 'logs', base='main'), 0)


if __name__ == '__main__':
    unittest.main()
