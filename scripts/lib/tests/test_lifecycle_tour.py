"""Model and transport checks without a firmware image."""
import collections
import itertools
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lifecycle_tour as tour


class FakeTransport:
    def __init__(self, fault=None):
        self.sent = []
        self.pending = []
        self.fault = fault

    def send(self, line):
        self.sent.append(line)
        if line == "APPS":
            self.pending += ["APPS name=Model A running=0 console=model", "APPS_END"]
        elif line == "model scenes":
            self.pending += ["MODEL scene=key0 current=1", "MODEL scene=key1 current=0", "MODEL_END"]
        elif line == "BUILDID":
            if self.fault != "heartbeat":
                self.pending += ["BUILD_ID=fixture"]
        elif line.startswith("OPEN "):
            self.pending += [{"error": "OPEN_ERR refused", "reboot": "ESP-ROM:esp32s3-test"}.get(self.fault, "OPEN_OK")]
        elif line == "HOME":
            self.pending += ["HOME_OK"]
        elif line.startswith("model scene "):
            self.pending += ["MODEL_END"]

    def lines(self, seconds=None):
        while self.pending:
            yield self.pending.pop(0)


class TourTests(unittest.TestCase):
    def model(self, count=3):
        return [tour.App(f"Model {i}", f"prefix{i}", tuple(f"key{j}" for j in range(count)))
                for i in range(count)]

    def test_apps_fields_and_spaces(self):
        self.assertEqual(tour.parse_apps(["APPS name=Model A running=1 console=model", "APPS name=Model B running=0"]),
                         [{"name": "Model A", "running": True, "console": "model"},
                          {"name": "Model B", "running": False}])

    def test_discovery_and_decline(self):
        transport = FakeTransport()
        self.assertEqual(tour.discover(transport)[0], tour.App("Model A", "model", ("key0", "key1")))
        self.assertEqual(tour.parse_scenes(["MODEL_ERR not handled"], "model"), ())
        for lines in (["MODEL_END"], ["MODEL scene=key0"],
                      ["MODEL scene=key0", "MODEL scene=key0", "MODEL_END"],
                      ["MODEL invalid", "MODEL_END"], ["MODEL_ERR not running"]):
            with self.subTest(lines=lines), self.assertRaises(tour.TourError):
                tour.parse_scenes(lines, "model")

    def test_empty_graphs_fail(self):
        for apps in ([], [tour.App("Model")], [tour.App("Model", "model", ("key0",))]):
            with self.assertRaises(tour.TourError):
                tour.plan(apps, tour.quarter_tilts())

    def test_scene_circuit_every_ordered_pair_once(self):
        for count in range(2, 9):
            keys = tuple(f"key{i}" for i in range(count))
            walk = tour.scene_walk(keys)
            self.assertEqual(walk[0], walk[-1])
            self.assertEqual(collections.Counter(zip(walk, walk[1:])),
                             collections.Counter(itertools.permutations(keys, 2)))

    def test_plan_apps_pairs_and_tilts(self):
        apps = self.model()
        steps = tour.plan(apps, tour.quarter_tilts())
        commands = [step.command for step in steps]
        expected = []
        for app in apps:
            expected += [f"OPEN {app.name}", "HOME"]
        for a, b in itertools.permutations(apps, 2):
            expected += [f"OPEN {a.name}", f"OPEN {b.name}"]
        self.assertEqual(commands[:len(expected)], expected)
        tilted = [step for step in steps if step.tilt]
        for index, step in enumerate(tilted):
            self.assertEqual(step.tilt, tour.quarter_tilts()[index % len(tour.quarter_tilts())])
        self.assertEqual(commands[-1], "IMU release")

    def test_runner_and_immediate_tilt(self):
        transport = FakeTransport()
        self.assertTrue(tour.run(transport, emit=lambda line: None))
        for index, line in enumerate(transport.sent):
            if line.startswith(("OPEN ", "HOME", "model scene ")):
                self.assertTrue(transport.sent[index + 1].startswith("IMU "))
        self.assertEqual(transport.sent[-2:], ["IMU release", "BUILDID"])

    def test_runner_stops_on_errors_heartbeat_and_reboot(self):
        for fault in ("error", "heartbeat", "reboot"):
            with self.subTest(fault=fault):
                transport = FakeTransport(fault)
                self.assertFalse(tour.run(transport, emit=lambda line: None))
                self.assertNotIn("model scenes", transport.sent)
