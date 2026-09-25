"""Board-free JSON output tests for read commands."""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


def setUpModule():
    global saved_hook
    saved_hook = os.environ.pop("AUTANA_LOCK_HOOK", None)


def tearDownModule():
    if saved_hook is not None:
        os.environ["AUTANA_LOCK_HOOK"] = saved_hook

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import autana  # noqa: E402


class JsonReadTests(unittest.TestCase):
    def output(self, command, args):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            self.assertEqual(command(args), 0)
        return json.loads(stream.getvalue())

    def test_buildid_reply(self):
        self.assertEqual(autana.parse_buildid("BUILD_ID=abc123"), {"build_id": "abc123"})
        with mock.patch.object(autana, "send", return_value=(0, ["BUILD_ID=abc123"])):
            self.assertEqual(self.output(autana.buildid, ["--json"]), {"build_id": "abc123"})

    def test_apps_reply(self):
        replies = ["APPS name=Star Chart running=1", "APPS name=Sand running=0", "APPS_END"]
        self.assertEqual(autana.parse_apps(replies), [{"name": "Star Chart", "running": True},
                                                       {"name": "Sand", "running": False}])
        with mock.patch.object(autana, "send", return_value=(0, replies)):
            self.assertEqual(self.output(autana.apps, ["--json"]),
                             {"apps": [{"name": "Star Chart", "running": True},
                                       {"name": "Sand", "running": False}]})

    def test_tune_reply(self):
        replies = ["TUNE ridge.trail=200 min=0 max=255 default=226", "TUNE_END count=1"]
        self.assertEqual(autana.parse_tunables(replies), [("ridge.trail", "200", "0", "255", "226")])
        with mock.patch.object(autana, "send", return_value=(0, replies)):
            self.assertEqual(self.output(autana.tune, ["--json"]),
                             {"tunables": [{"name": "ridge.trail", "value": 200, "min": 0,
                                            "max": 255, "default": 226}]})

    def test_status_reply(self):
        reply = "held by worker for capture since 123\nwaiting: Alice, Bob\n"
        self.assertEqual(autana.parse_status(reply),
                         {"state": "held", "owner": "worker", "purpose": "capture",
                          "acquired_at": 123, "waiting": ["Alice", "Bob"]})
        with mock.patch.object(autana.subprocess, "run", return_value=mock.Mock(returncode=0, stdout=reply)):
            self.assertEqual(self.output(autana.status, ["--json"]),
                             {"state": "held", "owner": "worker", "purpose": "capture",
                              "acquired_at": 123, "waiting": ["Alice", "Bob"]})

    def test_suite_list(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "launcher" / "test" / "suite.c"
            source.parent.mkdir(parents=True)
            source.write_text("SUITE_REGISTER(run_example_suite)\n")
            with mock.patch.object(autana, "engine_worktree", return_value=temp):
                self.assertEqual(self.output(autana.suite, ["list", "--json"]),
                                 {"suites": [{"name": "run_example_suite",
                                              "source": "launcher/test/suite.c",
                                              "on_request": False, "device_only": False}]})

    def test_id(self):
        result = self.output(autana.identify, ["--json"])
        self.assertEqual(result["owner"], autana.owner())
        self.assertEqual(result["pid"], autana.os.getpid())

    def test_text_output_is_the_board_reply_unchanged(self):
        replies = ["APPS name=Sand running=1 extra=kept", "APPS_END"]
        stream = io.StringIO()
        with mock.patch.object(autana, "send", return_value=(0, replies)), \
                contextlib.redirect_stdout(stream):
            autana.apps([])
        self.assertEqual(stream.getvalue(), "APPS name=Sand running=1 extra=kept\n")

    def test_json_rejected_for_tune_write(self):
        with self.assertRaises(SystemExit):
            autana.tune(["save", "--json"])


if __name__ == "__main__":
    unittest.main()
