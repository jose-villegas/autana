"""The capture tool pins board selection and resets arms without real device I/O."""
import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('sound_onset_capture', Path(__file__).resolve().parents[1] / 'sound_onset_capture.py')
onset = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(onset)


class OnsetCaptureTest(unittest.TestCase):
    def test_arms_are_independent_and_combined(self):
        arms = onset.arms(['sound.amp_mute=1', 'sound.dac_ramp=4'])
        self.assertEqual(len(arms), 4)
        self.assertEqual(arms['sound.amp_mute']['sound.dac_ramp'], 0)
        self.assertEqual(arms['combined']['sound.dac_ramp'], 4)
        self.assertTrue(all(value == 0 for value in arms['baseline'].values()))
        with self.assertRaises(ValueError):
            onset.arms(['other=1'])

    def test_capture_sequence_and_cleanup_on_failure(self):
        for fail in (False, True):
            calls = []

            def run(command, **kwargs):
                self.assertEqual(command[1:3], ['--board', 'TEST'])
                command = command[3:]
                calls.append(command)
                if command[0] == 'status':
                    reply = '{}'
                elif command[0] == 'buildid':
                    reply = json.dumps({'build_id': 'test-dev'})
                elif command[0] == 'tune':
                    reply = f'{command[1]}={command[2]}'
                else:
                    if fail:
                        raise ValueError('capture failed')
                    event = command[-1]
                    reply = f'SOUND onset event={event} hz=500 gain=33 pre_db=-60.0 peak_db=-50.0 excess_db=10.0 at_ms=20\n{onset.SOUND_UNTIL[0]}'
                return argparse.Namespace(stdout=reply)

            with tempfile.TemporaryDirectory() as directory:
                args = argparse.Namespace(board='TEST', runs=2, out=Path(directory)/'capture.txt', set=['sound.amp_mute=1'], autana='autana')
                with mock.patch.dict(onset.os.environ, {'_AUTANA_DEVICE_LOCK_TOKEN': 'test'}), mock.patch.object(onset.subprocess, 'run', side_effect=run):
                    if fail:
                        with self.assertRaisesRegex(ValueError, 'capture failed'):
                            onset.capture(args)
                    else:
                        onset.capture(args)
                        self.assertEqual(sum(c[:2] == ['sound', 'onset'] for c in calls), len(onset.EVENTS) * args.runs * 2)
                self.assertEqual(calls[0], ['status', '--json'])
                self.assertEqual(calls[-2:], [['status', '--json'], ['buildid', '--json']])
                self.assertEqual(calls[-5:-2], [['tune', name, '0'] for name in onset.DEFAULTS])


if __name__ == '__main__':
    unittest.main()
