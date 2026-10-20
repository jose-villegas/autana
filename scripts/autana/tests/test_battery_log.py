"""Persistent battery history wire contract, without a board."""
import contextlib
import io
import json
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'device' / 'tests'))
import isolation
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import autana


class BatteryLogTests(unittest.TestCase):
    row = 'BATLOG seq=16777215 up_min=1023 mv=16383 percent=127 usb=1 battery=1 charge=cc flow=charging reason=boot'

    def test_log_parse(self):
        result = autana.parse_battery_log([self.row, 'BATLOG end count=1'])
        self.assertEqual(result[0]['seq'], 16777215)
        self.assertEqual(result[0]['reason'], 'boot')
        self.assertTrue(result[0]['usb'])
        self.assertEqual(autana.parse_battery_log(['BATLOG end count=0']), [])
        for lines in ([self.row], ['BATLOG end count=1'], [self.row, 'BATLOG end count=0'],
                      [self.row.replace('seq=16777215', 'seq=16777216'), 'BATLOG end count=1'],
                      [self.row.replace('reason=boot', 'reason=bogus'), 'BATLOG end count=1'],
                      [self.row + ' extra', 'BATLOG end count=1'], ['BATLOG error read']):
            with self.assertRaises(ValueError):
                autana.parse_battery_log(lines)

    def test_shutdown_warning(self):
        row = self.row.replace('reason=boot', 'reason=shutdown-warning')
        self.assertEqual(autana.parse_battery_log([row, 'BATLOG end count=1'])[0]['reason'], 'shutdown-warning')

    def test_log_command(self):
        with mock.patch.object(autana, 'send', return_value=(0, [self.row, 'BATLOG end count=1'])) as send:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(autana.battery(['--log', '--json']), 0)
            self.assertEqual(json.loads(output.getvalue())[0]['reason'], 'boot')
            send.assert_called_once_with('BATTERY LOG', reply='BATLOG', until=['BATLOG end', 'BATLOG error'])
        with mock.patch.object(autana, 'send', return_value=(0, ['BATLOG cleared'])) as send:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(autana.battery(['--log', '--clear']), 0)
            send.assert_called_once_with('BATTERY LOG CLEAR', reply='BATLOG')
        for args in (['--clear'], ['--log', '--log'], ['--log', '--clear', '--json']):
            with self.assertRaises(SystemExit):
                autana.battery(args)

    def test_firmware_ranges(self):
        root = Path(__file__).resolve().parents[3]
        header = (root / 'launcher/main/power/power_log.h').read_text(encoding='utf-8')
        self.assertEqual(autana.BATTERY_LOG_SLOTS, int(re.search(r'#define POWER_LOG_SLOTS\s+([0-9]+)', header).group(1)))
        for field, name in (('seq', 'SEQ'), ('up_min', 'UPTIME'), ('mv', 'MV'), ('percent', 'PERCENT')):
            bits = int(re.search(rf'#define POWER_LOG_{name}_BITS\s+([0-9]+)', header).group(1))
            self.assertEqual(autana.BATTERY_LOG_LIMITS[field], (1 << bits) - 1)

    def test_firmware_names(self):
        root = Path(__file__).resolve().parents[3]
        reasons = re.findall(r'return "([^"]+)"', (root / 'launcher/main/power/power_log.c').read_text())
        decode = (root / 'launcher/main/power/power_decode.c').read_text()
        charges, flows = decode.split('power_flow_name', 1)
        for reason in reasons:
            if reason == 'unknown':
                continue
            for charge in re.findall(r'return "([^"]+)"', charges):
                for flow in re.findall(r'return "([^"]+)"', flows):
                    row = self.row.replace('reason=boot', f'reason={reason}').replace('charge=cc', f'charge={charge}').replace('flow=charging', f'flow={flow}')
                    self.assertEqual(autana.parse_battery_log([row, 'BATLOG end count=1'])[0]['reason'], reason)
