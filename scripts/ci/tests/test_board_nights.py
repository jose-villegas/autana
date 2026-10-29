"""board_nights.py starts one board night per listed host and refuses a list that would run no board."""
from pathlib import Path
import sys
import unittest

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import board_nights  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / '.github' / 'workflows'


class BoardNightsTests(unittest.TestCase):
    def test_each_host_gets_its_own_run_with_the_nights_marker(self):
        sent = []
        board_nights.start(['HostA', 'HostB'], 'main', '2026-10-30T03:00:00+01:00', 'abc', '3',
                           request=lambda method, path, body: sent.append((method, path, body)))
        self.assertEqual([body['inputs']['host'] for _, _, body in sent], ['HostA', 'HostB'])
        for method, path, body in sent:
            self.assertEqual((method, path), ('POST', '/actions/workflows/nightly-board.yml/dispatches'))
            self.assertEqual(body['ref'], 'main')
            self.assertEqual(body['inputs']['marker'], '2026-10-30T03:00:00+01:00')
            self.assertEqual((body['inputs']['from'], body['inputs']['max_steps']), ('abc', '3'))

    def test_unset_inputs_are_left_to_the_board_runs_defaults(self):
        sent = []
        board_nights.start(['HostA'], 'main', 'm', request=lambda method, path, body: sent.append(body))
        self.assertEqual(sent[0]['inputs'], {'host': 'HostA', 'marker': 'm'})

    def test_a_list_that_would_run_no_board_or_one_twice_is_refused(self):
        for text in (None, '', 'HostA', '[]', '{"a": 1}', '[""]', '[1]', '["HostA", "hosta"]'):
            with self.subTest(text=text), self.assertRaises(SystemExit):
                board_nights.hosts(text)
        self.assertEqual(board_nights.hosts('["HostA", "HostB"]'), ['HostA', 'HostB'])

    def test_the_board_run_takes_what_the_starter_sends(self):
        board = yaml.safe_load((WORKFLOWS / board_nights.BOARD_NIGHT_WORKFLOW).read_text())
        inputs = board[True]['workflow_dispatch']['inputs']
        self.assertTrue(inputs['host']['required'] and inputs['marker']['required'])
        self.assertIn('from', inputs)
        self.assertIn('max_steps', inputs)
        night = board['jobs']['nightly']
        self.assertEqual(night['runs-on'], ['self-hosted', 'windows', 'board', '${{ inputs.host }}'])
        self.assertEqual(night['env']['MARKER'], '${{ inputs.marker }}')
        self.assertEqual(board['concurrency'], {'group': 'nightly-board-${{ inputs.host }}', 'cancel-in-progress': True})

    def test_the_night_starts_the_board_runs_after_board_start(self):
        night = yaml.safe_load((WORKFLOWS / 'nightly-perf.yml').read_text())
        boards = night['jobs']['boards']
        self.assertEqual(boards['needs'], ['marker', 'wait'])
        self.assertEqual(boards['permissions']['actions'], 'write')
        self.assertEqual(boards['env']['BOARD_HOSTS'], '${{ vars.BOARD_HOSTS }}')
        self.assertTrue(any('scripts/ci/board_nights.py' in step.get('run', '') for step in boards['steps']))
        self.assertNotIn('nightly', night['jobs'], 'board lanes run only in the per-host board runs')


if __name__ == '__main__':
    unittest.main()
