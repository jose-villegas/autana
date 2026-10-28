"""autana sound: lines forwarded to the SOUND verb, its errors, and the shell's sound order around an app."""
import contextlib
import io
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device" / "tests"))
import isolation  # noqa: E402,F401

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import autana


class SoundTests(unittest.TestCase):
    def test_shell_closes_sound_before_exit_and_opens_before_enter(self):
        source = (Path(__file__).resolve().parents[3] / 'launcher/main/shell/shell_apps.c').read_text()
        exiting = source.split('shell_exit_app(const app_t** current) {', 1)[1].split('\n}', 1)[0]
        self.assertLess(exiting.index('sound_stop_app_voices();'), exiting.index('(*current)->exit();'))
        self.assertLess(exiting.index('sound_stop_app_voices();'), exiting.index('shell_systems_app_exit();'))
        self.assertLess(exiting.index('shell_systems_app_exit();'), exiting.index('sound_end_app();'))
        entering = source.split('shell_start_app(const app_t** current, const app_t* next) {', 1)[1].split('\n}', 1)[0]
        self.assertEqual(1, entering.count('sound_begin_app();'))
        self.assertLess(entering.index('sound_begin_app();'), entering.index('(*current)->enter();'))

    def test_sound_is_drive_command(self):
        self.assertIn('autana sound play <pack>/<name> [loop] [gain]', autana.help_text(['drive']))

    def test_flat_play_dispatch(self):
        self.assertIn('autana play <pack>/<name> [loop] [gain]', autana.help_text(['drive']))
        with patch.object(autana, 'send', return_value=(0, ['SOUND_END voice=1'])) as send:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, autana.COMMANDS['play'](['music/theme', 'loop', '50']))
            self.assertEqual(send.call_args.args[0], 'SOUND play music/theme loop 50')

    def test_sound_forwards_commands(self):
        for args in (['play', 'tap'], ['play', 'tap', 'loop', '50'],
                     ['play', 'tap', '25'], ['stop'], ['stop', 'all'],
                     ['stop', '123'], ['stats']):
            with self.subTest(args=args), patch.object(autana, 'send', return_value=(0, ['SOUND ok'])) as send:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(0, autana.sound(args))
                send.assert_called_once_with('SOUND ' + ' '.join(args), reply='SOUND',
                    until=['SOUND' + '_END', 'SOUND' + '_ERR'])

    def test_sound_forwards_bad_args_for_firmware_validation(self):
        for args in ([], ['play'], ['play', 'tap', '101'], ['play', 'tap', '-1'],
                     ['play', 'tap', 'loop', 'loop'], ['stop', '-1'], ['stats', 'extra']):
            with self.subTest(args=args), patch.object(autana, 'send', return_value=(0, ['SOUND_ERR invalid'])) as send:
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(1, autana.sound(args))
                self.assertEqual('SOUND ' + ' '.join(args), send.call_args.args[0])

    def test_sound_reports_error_and_transport_failure(self):
        for result in ((0, ['SOUND_ERR full']), (0, []), (2, [])):
            with self.subTest(result=result), patch.object(autana, 'send', return_value=result):
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertNotEqual(0, autana.sound(['stats']))


class CodecTests(unittest.TestCase):
    def test_codec_ends_on_its_one_reply_line(self):
        with patch.object(autana, 'send', return_value=(0, ['CODEC reg=0x1B value=0x0A'])) as send:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(0, autana.COMMANDS['codec'](['0x1b']))
        self.assertEqual(send.call_args.args[0], 'codec 0x1b')
        self.assertEqual(send.call_args.kwargs, {'reply': 'CODEC'})
        self.assertEqual(out.getvalue().strip(), 'CODEC reg=0x1B value=0x0A')

    def test_codec_errors_fail(self):
        for line in ('CODEC error: codec unavailable or register access failed', 'CODEC usage: codec <reg> [value]'):
            with patch.object(autana, 'send', return_value=(0, [line])):
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(1, autana.COMMANDS['codec'](['0x1b', '0x0a']))
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(2, autana.COMMANDS['codec']([]))

class MicWriteTests(unittest.TestCase):
    def test_scheduled_writes_forwarded(self):
        with patch.object(autana, 'refused_as_busy', return_value=False), \
             patch.object(autana, 'device_command', return_value=['device', 'mic-record']), \
             patch.object(autana, 'call_device', return_value=0) as call:
            self.assertEqual(0, autana.mic(['record', '3000', '--settle', '500', '--at', '1000', '0x32', '40',
                                         '--at', '2000', '0x32', '0', '--at', '2500', 'tone', '202', '60', '60', '-o', 'take.wav', '--json']))
        words = call.call_args.args[0]
        self.assertEqual(words[words.index('--at'):],
                         ['--at', '1000', '50', '40', '--at', '2000', '50', '0', '--at', '2500', 'tone', '202', '60', '60', '--json'])
