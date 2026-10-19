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
    def test_explicit_arms(self):
        self.assertEqual(onset.arms(['off:sound.declick_us=0', 'short:sound.declick_us=2000']),
                         {'off': {'sound.declick_us': 0}, 'short': {'sound.declick_us': 2000}})
        for settings in ([], ['bad:other=1'], ['x:sound.declick_us=1', 'x:sound.declick_us=2'],
                         ['x:sound.declick_us=-1'], ['x:sound.declick_us=1000001'],
                         ['meta:sound.declick_us=0'], [':sound.declick_us=0']):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                onset.arms(settings)

    def test_failed_runs_retry_once_and_keep_stderr(self):
        for failures in (1, 2):
            attempts = 0

            def run(command, **kwargs):
                nonlocal attempts
                self.assertTrue(kwargs['capture_output'])
                if 'onset' in command:
                    attempts += 1
                    if attempts <= failures:
                        raise onset.subprocess.CalledProcessError(1, command, output='partial', stderr='SOUND_ERR capture failed')
                    reply = f'SOUND onset event={command[-1]} hz=500 gain=33 pre_db=-60.0 peak_db=-50.0 excess_db=10.0 at_ms=20\n{onset.SOUND_UNTIL[0]}'
                elif 'tune' in command:
                    reply = f'{command[-2]}={command[-1]}'
                elif 'buildid' in command:
                    reply = '{"build_id": "test-dev"}'
                else:
                    reply = '{}'
                return argparse.Namespace(stdout=reply, stderr='capture diagnostic' if 'onset' in command else '')

            with tempfile.TemporaryDirectory() as directory:
                args = argparse.Namespace(board='TEST', runs=1, out=Path(directory)/'capture.txt',
                                          arm=['off:sound.declick_us=0'], autana='autana')
                with mock.patch.dict(onset.os.environ, {'_AUTANA_DEVICE_LOCK_TOKEN': 'test'}), mock.patch.object(onset.subprocess, 'run', side_effect=run):
                    onset.capture(args)
                text = args.out.read_text()
                self.assertEqual(attempts, len(onset.EVENTS) + 1)
                self.assertEqual(text.count('off\tfailure sound onset amp-on\t'), failures)
                self.assertIn('SOUND_ERR capture failed', text)
                self.assertIn('partial', text)
                self.assertIn('off\tstderr sound onset start\t"capture diagnostic"', text)
                self.assertEqual(text.count('off\tsound onset amp-on\t'), int(failures == 1))

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
                return argparse.Namespace(stdout=reply, stderr='')

            with tempfile.TemporaryDirectory() as directory:
                args = argparse.Namespace(board='TEST', runs=2, out=Path(directory)/'capture.txt', arm=['off:sound.declick_us=0', 'short:sound.declick_us=2000'], autana='autana')
                with mock.patch.dict(onset.os.environ, {'_AUTANA_DEVICE_LOCK_TOKEN': 'test'}), mock.patch.object(onset.subprocess, 'run', side_effect=run):
                    if fail:
                        with self.assertRaisesRegex(ValueError, 'capture failed'):
                            onset.capture(args)
                    else:
                        onset.capture(args)
                        self.assertEqual(sum(c[:2] == ['sound', 'onset'] for c in calls), len(onset.EVENTS) * args.runs * 2)
                self.assertEqual(calls[0], ['status', '--json'])
                self.assertEqual(calls[-2:], [['status', '--json'], ['buildid', '--json']])
                self.assertEqual(calls[-3:-2], [['tune', name, str(value)] for name, value in onset.DEFAULTS.items()])


if __name__ == '__main__':
    unittest.main()
