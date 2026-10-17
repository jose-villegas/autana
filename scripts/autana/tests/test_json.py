"""Board-free JSON output tests for read commands."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device" / "tests"))
import isolation  # noqa: E402,F401  (first: keeps the suite out of real records)
import contextlib
import io
import json
import os
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
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "launcher" / "test" / "suite.c"
            source.parent.mkdir(parents=True)
            source.write_text("SUITE_REGISTER(run_example_suite)\n")
            with mock.patch.object(autana, "resolve_project", return_value=temp):
                self.assertEqual(self.output(autana.suite, ["list", "--json"]),
                                 {"suites": [{"name": "run_example_suite",
                                              "source": "launcher/test/suite.c",
                                              "on_request": False, "device_only": False}]})

    def test_text_output_is_the_board_reply_unchanged(self):
        replies = ["APPS name=B running=1 extra=kept", "APPS_END"]
        stream = io.StringIO()
        with mock.patch.object(autana, "send", return_value=(0, replies)), \
                contextlib.redirect_stdout(stream):
            autana.apps([])
        self.assertEqual(stream.getvalue(), "APPS name=B running=1 extra=kept\n")

    def test_json_rejected_for_tune_write(self):
        with self.assertRaises(SystemExit):
            autana.tune(["save", "--json"])


if __name__ == "__main__":
    unittest.main()
