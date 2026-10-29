"""Board-free JSON output tests for read commands."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device" / "tests"))
import isolation  # noqa: E402,F401  (first: keeps the suite out of real records)
import contextlib
import io
import json
import os
import re
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import autana  # noqa: E402


class JsonReadTests(unittest.TestCase):
    def output(self, command, args):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            self.assertEqual(command(args), 0)
        return json.loads(stream.getvalue())

    def test_battery_reply(self):
        reply = "BATTERY valid=1 usb=1 battery=1 mv=3987 percent=82 charge=cc flow=charging reg00=0x2a reg01=0x22"
        expected = dict(valid=True, usb=True, battery=True, mv=3987, percent=82,
                        charge="cc", flow="charging", reg00=42, reg01=34)
        self.assertEqual(autana.parse_battery(reply), expected)
        with mock.patch.object(autana, "send", return_value=(0, [reply])) as send:
            self.assertEqual(self.output(autana.COMMANDS["battery"], ["--json"]), expected)
            send.assert_called_once_with("BATTERY", reply="BATTERY")
        for bad in (reply + " trailing", reply.replace("percent=82", f"percent={autana.BATTERY_PERCENT_MAX + 1}"),
                    reply.replace("charge=cc", "charge=bogus"), "BATTERY error"):
            with self.assertRaises(ValueError):
                autana.parse_battery(bad)
        for args in (["extra"], ["--json", "--json"], ["--unknown"]):
            with self.assertRaises(SystemExit):
                autana.battery(args)
        for result, code in (((0, []), 1), ((7, []), 7)):
            with mock.patch.object(autana, "send", return_value=result):
                self.assertEqual(autana.battery([]), code)
        with mock.patch.object(autana, "send", return_value=(0, [reply])), contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(autana.battery([]), 0)
        self.assertEqual(out.getvalue(), reply + "\n")

    def test_battery_bounds_and_last_reply(self):
        reply = ("BATTERY valid=1 usb=1 battery=1 "
                 f"mv={autana.BATTERY_VOLTAGE_MAX_MV} percent={autana.BATTERY_PERCENT_MAX} "
                 "charge=cc flow=charging reg00=0xff reg01=0xff")
        self.assertEqual(autana.parse_battery(reply)["mv"], autana.BATTERY_VOLTAGE_MAX_MV)
        with self.assertRaises(ValueError):
            autana.parse_battery(reply.replace(f"mv={autana.BATTERY_VOLTAGE_MAX_MV}",
                                              f"mv={autana.BATTERY_VOLTAGE_MAX_MV + 1}"))
        zero = reply.replace("valid=1", "valid=0").replace(f"mv={autana.BATTERY_VOLTAGE_MAX_MV}", "mv=0")
        self.assertFalse(autana.parse_battery(zero)["valid"])
        with mock.patch.object(autana, "send", return_value=(0, ["BATTERY malformed", zero])):
            self.assertEqual(self.output(autana.battery, ["--json"]), autana.parse_battery(zero))
        with mock.patch.object(autana, "send", return_value=(0, [reply, "BATTERY malformed"])), \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(autana.battery(["--json"]), 1)
        self.assertIn("invalid BATTERY reply", errors.getvalue())

    def test_battery_firmware_names(self):
        import re
        source = (Path(__file__).resolve().parents[3] / "launcher" / "test" / "suites" / "suite_power.c").read_text()
        names = {}
        for kind in ("charges", "flows"):
            body = re.search(rf'const char\* const {kind}\[\] = \{{(.*?)\}};', source, re.S).group(1)
            names[kind] = re.findall(r'"([^"]+)"', body)
        for charge in names["charges"]:
            for flow in names["flows"]:
                reply = (f"BATTERY valid=1 usb=1 battery=1 mv=0 percent=0 charge={charge} "
                         f"flow={flow} reg00=0xff reg01=0xff")
                self.assertEqual(autana.parse_battery(reply)["charge"], charge)
                self.assertEqual(autana.parse_battery(reply)["flow"], flow)

    def test_heap_reply_and_failure(self):
        reply = "HEAP free=123456 largest=54321 total=200000"
        self.assertEqual(autana.parse_heap(reply), {"free": 123456, "largest": 54321, "total": 200000})
        with mock.patch.object(autana, "send", return_value=(0, [reply])) as send:
            self.assertEqual(self.output(autana.heap, ["--json"]),
                             {"free": 123456, "largest": 54321, "total": 200000})
            send.assert_called_once_with("HEAP", reply="HEAP")
        stream = io.StringIO()
        with mock.patch.object(autana, "send", return_value=(0, [reply])), contextlib.redirect_stdout(stream):
            self.assertEqual(autana.heap([]), 0)
        self.assertEqual(stream.getvalue(), reply + "\n")
        for result, expected in (((0, []), 1), ((7, []), 7)):
            with mock.patch.object(autana, "send", return_value=result):
                self.assertEqual(autana.heap([]), expected)
        for args in (["extra"], ["--json", "--json"], ["--unknown"]):
            with self.assertRaises(SystemExit):
                autana.heap(args)
        for reply in ("HEAP free=x largest=1", "HEAP free=1", "OTHER free=1 largest=1"):
            with self.assertRaises(ValueError):
                autana.parse_heap(reply)

    def test_metrics_are_top_level_only(self):
        for name in ("heap", "perf", "framewatch"):
            self.assertIs(autana.COMMANDS[name], getattr(autana, name))
        for name in ("heap", "perf"):
            with self.assertRaises(SystemExit):
                autana.debug([name])

    def test_debug_framewatch_alias_announces_once(self):
        reply = 'FRAMEWATCH {"frames":40,"allocs":0,"repeating":0,"sites":[]}'
        stream = io.StringIO()
        with mock.patch.object(autana, "send", return_value=(0, [reply])), \
                contextlib.redirect_stderr(stream):
            self.assertEqual(self.output(autana.debug, ["framewatch"]),
                             {"frames": 40, "allocs": 0, "repeating": 0, "sites": []})
        self.assertEqual(stream.getvalue().count("use `autana framewatch`"), 1)

    def test_tone_reply(self):
        for heard in (True, False):
            with self.subTest(heard=heard):
                reply = f"TONE {'heard' if heard else 'not heard'} 440 Hz: -21.5 dB (silence -63.0 dB), path acoustic, measured 440.02 Hz, open 8192 B DMA, thd -40.0 dB, peak -6.0 dBFS"
                expected = {"imu_gaps": None, "shake_reason": None, "pulse_ms": 0, "noise_dbfs": None, "hz": 440, "tone_db": -21.5, "silence_db": -63.0,
                            "heard": heard, "open_dma_bytes": 8192, "path": "acoustic",
                            "measured_hz": 440.02, "hz_error": 440.02 - 440, "thd_db": -40.0, "peak_dbfs": -6.0,
                            "shake_rms_mg": None, "shake_peak_mg": None, "silence_shake_rms_mg": None, "gx_mg": None, "gy_mg": None, "gz_mg": None}
                self.assertEqual(autana.parse_tone(reply), expected)
                with mock.patch.object(autana, "send", return_value=(0, [reply])) as send:
                    self.assertEqual(self.output(autana.COMMANDS["tone"], ["440", "200", "--json"]), expected)
                send.assert_called_once_with(
                    "TONE 440 200", reply="TONE",
                    seconds=autana.TONE_MARGIN_SECONDS + autana.TONE_CAPTURE_COUNT * 200 / autana.TONE_MS_PER_SECOND)

    def test_tone_short_capture_has_no_frequency(self):
        reply = "TONE heard 440 Hz: -21.5 dB (silence -63.0 dB), path internal, measured n/a, open 8192 B DMA, thd -40.0 dB, peak -6.0 dBFS"
        with mock.patch.object(autana, "send", return_value=(0, [reply])):
            result = self.output(autana.tone, ["440", "1", "--internal", "--json"])
        self.assertIsNone(result["measured_hz"])
        self.assertIsNone(result["hz_error"])

    def test_main_tone_reply(self):
        reply = "TONE heard 440 Hz: -21.5 dB (silence -63.0 dB), path acoustic, measured 440.02 Hz, open 8192 B DMA"
        result = autana.parse_tone(reply)
        self.assertIsNone(result["thd_db"])
        self.assertIsNone(result["peak_dbfs"])
        self.assertEqual(result["hz"], 440)

    def test_tone_shake(self):
        base = "TONE heard 60 Hz: -40.0 dB (silence -63.0 dB), path acoustic, measured 60.01 Hz, open 0 B DMA, thd -10.0 dB, peak -6.0 dBFS"
        result = autana.parse_tone(base + ", shake 12.50 mg rms 30.00 mg peak (silence 0.25 mg rms)")
        self.assertEqual((result["shake_rms_mg"], result["shake_peak_mg"], result["silence_shake_rms_mg"]),
                         (12.5, 30.0, 0.25))
        gravity = autana.parse_tone(base + ", shake 12.50 mg rms 30.00 mg peak (silence 0.25 mg rms), gx_mg -12.50, gy_mg 25.00, gz_mg 999.00")
        self.assertEqual((gravity["gx_mg"], gravity["gy_mg"], gravity["gz_mg"]), (-12.5, 25, 999))
        result = autana.parse_tone(base + ", shake n/a")
        self.assertIsNone(result["shake_rms_mg"])
        with self.assertRaises(ValueError):
            autana.parse_tone(base + ", shake 12.50 mg rms")

    def test_tone_gap_reason(self):
        base = "TONE heard 202 Hz: -40.0 dB (silence -63.0 dB), path acoustic, measured n/a, open 0 B DMA, shake n/a"
        reply = base + ", imu_gaps 2, shake_reason gap inside pulse"
        with mock.patch.object(autana, "send", return_value=(0, [reply])):
            result = self.output(autana.tone, ["202", "400", "--pulse", "30", "--json"])
        self.assertEqual(result["imu_gaps"], 2)
        self.assertEqual(result["shake_reason"], "gap inside pulse")
        self.assertIsNone(autana.parse_tone(base)["imu_gaps"])
        self.assertIsNone(autana.parse_tone(base)["shake_reason"])

    def test_tone_noise_and_pulse_fields(self):
        reply = ("TONE heard 202 Hz: -40.0 dB (silence -63.0 dB), noise -70.0 dBFS, "
                 "path acoustic, measured n/a, open 0 B DMA, thd n/a dB, peak -6.0 dBFS, pulse_ms 60, "
                 "shake 12.50 mg rms 30.00 mg peak (silence 0.25 mg rms), "
                 "gx_mg -12.50, gy_mg 25.00, gz_mg 999.00, imu_gaps 2, shake_reason gap inside pulse")
        result = autana.parse_tone(reply)
        self.assertEqual(result["noise_dbfs"], -70.0)
        self.assertEqual(result["pulse_ms"], 60)
        self.assertEqual(result["peak_dbfs"], -6.0)
        self.assertEqual(result["shake_peak_mg"], 30.0)
        self.assertEqual((result["gx_mg"], result["gy_mg"], result["gz_mg"]), (-12.5, 25.0, 999.0))
        self.assertEqual(result["imu_gaps"], 2)
        self.assertEqual(result["shake_reason"], "gap inside pulse")

    def test_tone_pulse(self):
        reply = "TONE heard 202 Hz: -40.0 dB (silence -63.0 dB), path internal, measured n/a, open 0 B DMA, thd n/a dB, peak -6.0 dBFS, pulse_ms 60, shake 12.50 mg rms 30.00 mg peak (silence 0.25 mg rms)"
        with mock.patch.object(autana, "send", return_value=(0, [reply])) as send:
            result = self.output(autana.tone, ["202", "200", "30", "--pulse", "60", "--internal", "--json"])
        self.assertEqual(send.call_args.args, ("TONE 202 200 30 pulse 60 internal",))
        self.assertEqual(result["pulse_ms"], 60)
        self.assertEqual(result["shake_peak_mg"], 30)
        for options in (["--pulse"], ["--pulse", "0"], ["--pulse", "201"],
                        ["--pulse", "-1"], ["--pulse", "30", "--pulse", "60"]):
            with mock.patch.object(autana, "send") as send, self.assertRaises(SystemExit):
                autana.tone(["202", "200", *options])
            send.assert_not_called()

    def test_tone_unavailable_thd(self):
        reply = "TONE heard 8000 Hz: -21.5 dB (silence -63.0 dB), path acoustic, measured n/a, open 8192 B DMA, thd n/a dB, peak -0.1 dBFS"
        result = autana.parse_tone(reply)
        self.assertIsNone(result["thd_db"])
        self.assertEqual(result["peak_dbfs"], -0.1)

    def test_tone_internal_flags(self):
        reply = "TONE heard 440 Hz: -21.5 dB (silence -63.0 dB), path internal, measured 443.70 Hz, open 8192 B DMA, thd -40.0 dB, peak -6.0 dBFS"
        for numbers in (["440", "200"], ["440", "200", "30"]):
            for flags in (["--internal", "--json"], ["--json", "--internal"]):
                with mock.patch.object(autana, "send", return_value=(0, [reply])) as send:
                    result = self.output(autana.tone, numbers + flags)
                self.assertEqual(send.call_args.args, ("TONE " + " ".join(numbers) + " internal",))
                self.assertEqual(result["path"], "internal")
                self.assertAlmostEqual(result["hz_error"], 3.7)

    def test_tone_malformed_and_error(self):
        for reply in ("TONE usage: tone <hz> <ms> [volume]", "TONE error: codec open failed",
                      "TONE heard", "OTHER heard 440 Hz",
                      "TONE heard 440 Hz: nan dB (silence -63.0 dB), open 8192 B DMA, thd -40.0 dB, peak -6.0 dBFS",
                      "TONE heard 440 Hz: -21.5 dB (silence -63.0 dB), open 8192 B DMA trailing"):
            with self.subTest(reply=reply), self.assertRaises(ValueError):
                autana.parse_tone(reply)
            with mock.patch.object(autana, "send", return_value=(0, [reply])), \
                    contextlib.redirect_stderr(io.StringIO()) as stderr, \
                    contextlib.redirect_stdout(io.StringIO()) as stdout:
                self.assertEqual(autana.COMMANDS["tone"](["440", "200", "--json"]), 1)
            self.assertEqual(stdout.getvalue(), "")
            if reply.startswith(("TONE usage:", "TONE error:")):
                self.assertEqual(stderr.getvalue(), reply + "\n")
        for result, expected in (((0, []), 1), ((7, []), 7)):
            with mock.patch.object(autana, "send", return_value=result):
                self.assertEqual(autana.COMMANDS["tone"](["440", "200"]), expected)

    def test_tone_forwards_decimal_args_and_plain_output(self):
        reply = "TONE not heard 1 Hz: -63.0 dB (silence -63.0 dB), path acoustic, measured 1.00 Hz, open 8192 B DMA, thd -40.0 dB, peak -6.0 dBFS"
        for args in (("0", "0"), ("999999", "999999", "999999"), ("00440", "00200", "030")):
            ms = int(args[1])
            stream = io.StringIO()
            with mock.patch.object(autana, "send", return_value=(0, [reply])) as send, \
                    contextlib.redirect_stdout(stream):
                self.assertEqual(autana.COMMANDS["tone"]([*args]), 0)
            self.assertEqual(stream.getvalue(), reply + "\n")
            self.assertEqual(send.call_args.args, ("TONE " + " ".join(args),))
            self.assertEqual(send.call_args.kwargs["seconds"],
                             autana.TONE_MARGIN_SECONDS + autana.TONE_CAPTURE_COUNT * ms / autana.TONE_MS_PER_SECOND)

    def test_tone_rejects_invalid_args_before_send(self):
        for args in ([], ["440"], ["440", "200", "30", "extra"], ["440", ""],
                     ["440", "200", "--json", "30"], ["440", "200", "--internal", "30"],
                     ["-1", "200"], ["+440", "200"], ["440.0", "200"],
                     ["\u0664\u0664\u0660", "200"], ["440", "200", "-1"], ["440", "200", "oops"],
                     ["440", "200", "--unknown"], ["440", "200", "--json", "--json"], ["440", "200", "--internal", "--internal"]):
            with self.subTest(args=args), mock.patch.object(autana, "send") as send:
                with self.assertRaises(SystemExit):
                    autana.COMMANDS["tone"]([*args])
                send.assert_not_called()

    def test_buildid_reply(self):
        self.assertEqual(autana.parse_buildid("BUILD_ID=abc123"), {"build_id": "abc123"})
        with mock.patch.object(autana, "send", return_value=(0, ["BUILD_ID=abc123"])):
            self.assertEqual(self.output(autana.buildid, ["--json"]), {"build_id": "abc123"})

    def test_framewatch_reply_is_the_boards_json(self):
        reply = 'FRAMEWATCH {"frames":40,"allocs":0,"repeating":0,"sites":[]}'
        with mock.patch.object(autana, "send", return_value=(0, [reply])):
            self.assertEqual(self.output(autana.framewatch, []),
                             {"frames": 40, "allocs": 0, "repeating": 0, "sites": []})

    def test_apps_console_prefix(self):
        self.assertEqual(autana.parse_apps(["APPS name=Model running=0 console=model"]),
                         [{"name": "Model", "running": False, "console": "model"}])

    def test_malformed_apps_json_reports_error(self):
        with mock.patch.object(autana, "send", return_value=(0, ["APPS malformed", "APPS_END"])):
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                self.assertEqual(autana.apps(["--json"]), 1)
            self.assertIn("malformed APPS", errors.getvalue())

    def test_apps_reply(self):
        replies = ["APPS name=A running=1", "APPS name=B running=0", "APPS_END"]
        self.assertEqual(autana.parse_apps(replies), [{"name": "A", "running": True},
                                                       {"name": "B", "running": False}])
        with mock.patch.object(autana, "send", return_value=(0, replies)):
            self.assertEqual(self.output(autana.apps, ["--json"]),
                             {"apps": [{"name": "A", "running": True},
                                       {"name": "B", "running": False}]})

    def test_tune_reply(self):
        replies = ["TUNE tune.primary=200 min=0 max=255 default=226", "TUNE_END count=1"]
        self.assertEqual(autana.parse_tunables(replies), [("tune.primary", "200", "0", "255", "226")])
        with mock.patch.object(autana, "send", return_value=(0, replies)):
            self.assertEqual(self.output(autana.tune, ["--json"]),
                             {"tunables": [{"name": "tune.primary", "value": 200, "min": 0,
                                            "max": 255, "default": 226}]})

    def test_suite_list(self):
        replies = ["RUNSUITE_SUITE name=run_b_suite on_request=1", "RUNSUITE_SUITE name=run_a_suite on_request=0",
                   "RUNSUITE_END count=2"]
        with mock.patch.object(autana, "send", return_value=(0, replies)) as send:
            self.assertEqual(self.output(autana.suite, ["list", "--json"]),
                             {"suites": [{"name": "run_a_suite", "on_request": False},
                                         {"name": "run_b_suite", "on_request": True}]})
        send.assert_called_once_with("RUNSUITE ?", reply="RUNSUITE_", until=["RUNSUITE_END"])

    def test_suite_list_without_a_self_test_image_says_so(self):
        for code, replies in ((1, []), (0, ["RUNSUITE_SUITE name=a on_request=0", "RUNSUITE_END count=2"])):
            stderr = io.StringIO()
            with mock.patch.object(autana, "send", return_value=(code, replies)),                     contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                self.assertEqual(autana.suite(["list"]), 1)
            self.assertIn("autana suite list", stderr.getvalue())

    def test_text_output_is_the_board_reply_unchanged(self):
        replies = ["APPS name=B running=1 extra=kept", "APPS_END"]
        stream = io.StringIO()
        with mock.patch.object(autana, "send", return_value=(0, replies)), \
                contextlib.redirect_stdout(stream):
            autana.apps([])
        self.assertEqual(stream.getvalue(), "APPS name=B running=1 extra=kept\n")

    def test_json_rejected_for_tune_write(self):
        with self.assertRaises(SystemExit):
            autana.tune(["reset", "x", "--json"])


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


if __name__ == "__main__":
    unittest.main()
