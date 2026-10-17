import contextlib
import io
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import autana


class SoundTests(unittest.TestCase):
    def test_sound_is_drive_command(self):
        self.assertIn('autana sound play <name> [loop] [gain]', autana.help_text(['drive']))

    def test_sound_forwards_commands(self):
        for args in (['play', 'tap'], ['play', 'tap', 'loop', '50'],
                     ['play', 'tap', '25'], ['stop'], ['stop', 'all'],
                     ['stop', '123'], ['stats']):
            with self.subTest(args=args), patch.object(autana, 'send', return_value=(0, ['SOUND ok'])) as send:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(0, autana.sound(args))
                send.assert_called_once_with('SOUND ' + ' '.join(args), reply='SOUND',
                    until=['SOUND_END', 'SOUND error:', 'SOUND usage:'])

    def test_sound_rejects_bad_args(self):
        for args in ([], ['play'], ['play', 'tap', '101'], ['play', 'tap', '-1'],
                     ['play', 'tap', 'loop', 'loop'], ['stop', '-1'], ['stats', 'extra']):
            with self.subTest(args=args), patch.object(autana, 'send') as send:
                with self.assertRaises(SystemExit):
                    autana.sound(args)
                send.assert_not_called()

    def test_sound_reports_error_and_transport_failure(self):
        for result in ((0, ['SOUND error: full']), (0, []), (2, [])):
            with self.subTest(result=result), patch.object(autana, 'send', return_value=result):
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertNotEqual(0, autana.sound(['stats']))
