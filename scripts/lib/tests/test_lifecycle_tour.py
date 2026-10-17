"""Model and transport checks without a firmware image."""
import collections
import itertools
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lifecycle_tour as tour


class FakeTransport:
    def __init__(self, fault=None):
        self.sent = []
        self.pending = []
        self.fault = fault
        self.current = "key0"
        self.queued = None

    def send(self, line):
        self.sent.append(line)
        if self.fault == "port":
            raise OSError("disconnected")
        if line.startswith("IMU quarter "):
            self.pending += [f"IMU_OK quarter={line.split()[-1]} quarters=4 sample_ms=1"]
        elif line == "APPS":
            self.pending += ["APPS name=Model A running=0 console=model", "APPS_END"]
        elif line == "model scenes":
            if self.queued is not None and self.fault != "dropped":
                self.current = self.queued
            self.queued = None
            self.pending += [f"MODEL scene={key} current={int(key == self.current)}" for key in ("key0", "key1")] + ["MODEL_END"]
        elif line == "BUILDID":
            if self.fault != "heartbeat":
                self.pending += ["BUILD_ID=fixture"]
        elif line.startswith("OPEN "):
            self.pending += [{"error": "OPEN_ERR refused", "reboot": "ESP-ROM:esp32s3-test"}.get(self.fault, "OPEN_OK")]
        elif line == "HOME":
            self.queued = None
            self.pending += ["HOME_OK"]
        elif line.startswith("model scene "):
            self.queued = line.split()[-1]
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
        self.assertEqual(tour.discover(transport, itertools.cycle(("IMU quarter 0",)), emit=lambda line: None)[0], tour.App("Model A", "model", ("key0", "key1")))
        self.assertEqual(tour.parse_scenes(["MODEL_ERR not handled"], "model"), ())
        for lines in (["MODEL_END"], ["MODEL scene=key0"],
                      ["MODEL scene=key0", "MODEL scene=key0", "MODEL_END"],
                      ["MODEL invalid", "MODEL_END"], ["MODEL_ERR not running"]):
            with self.subTest(lines=lines), self.assertRaises(tour.TourError):
                tour.parse_scenes(lines, "model")

    def test_empty_graphs_fail(self):
        for apps in ([], [tour.App("Model")], [tour.App("Model", "model", ("key0",))]):
            with self.assertRaises(tour.TourError):
                tour.plan(apps, tuple(f"IMU quarter {q}" for q in range(4)))

    def test_scene_circuit_every_ordered_pair_once(self):
        for count in range(2, 9):
            keys = tuple(f"key{i}" for i in range(count))
            walk = tour.scene_walk(keys)
            self.assertEqual(walk[0], walk[-1])
            self.assertEqual(collections.Counter(zip(walk, walk[1:])),
                             collections.Counter(itertools.permutations(keys, 2)))

    def test_plan_apps_pairs_and_tilts(self):
        apps = self.model()
        steps = tour.plan(apps, itertools.cycle(f"IMU quarter {q}" for q in range(4)))
        commands = [step.command for step in steps]
        expected = []
        for app in apps:
            expected += [f"OPEN {app.name}", "HOME"]
        for a, b in itertools.permutations(apps, 2):
            expected += [f"OPEN {a.name}", f"OPEN {b.name}"]
        self.assertEqual(commands[:len(expected)], expected)
        tilted = [step for step in steps if step.tilt]
        for index, step in enumerate(tilted):
            self.assertEqual(step.tilt, tuple(f"IMU quarter {q}" for q in range(4))[index % len(tuple(f"IMU quarter {q}" for q in range(4)))])
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

    def test_dropped_deferred_scene_fails(self):
        transport = FakeTransport("dropped")
        self.assertFalse(tour.run(transport, emit=lambda line: None))

    def test_release_image_fails_at_once(self):
        transport = FakeTransport()
        transport.pending = ["W console: ignoring line: 'IMU quarter 0'"]
        messages = []
        self.assertFalse(tour.run(transport, emit=messages.append))
        self.assertTrue(any("needs a development image" in line for line in messages))

    def test_port_lost_and_timeout_are_verdicts(self):
        for fault in ("port", "timeout"):
            transport = FakeTransport(fault)
            if fault == "timeout":
                transport.deadline = 0
            messages = []
            self.assertFalse(tour.run(transport, emit=messages.append))
            expected = "port lost: the board reset or rebooted" if fault == "port" else "tour's time ran out"
            self.assertTrue(any(expected in line for line in messages), messages)

    def test_heartbeat_is_liveness_only(self):
        transport = FakeTransport()
        transport.pending = ["MODEL_ERR stale"]
        self.assertTrue(tour.heartbeat(transport))

    def test_log_prefixed_reply(self):
        transport = FakeTransport()
        transport.pending = ["I (23) console: HOME_OK"]
        self.assertEqual(tour.exchange(transport, tour.Step("HOME", "HOME")), ["HOME_OK"])

    def test_tilt_waits_for_firmware_sample_period(self):
        transport = FakeTransport()
        with mock.patch.object(tour.time, "monotonic", return_value=0) as clock, \
                mock.patch.object(tour.time, "sleep") as sleep:
            send = transport.send

            def delayed_send(line):
                send(line)
                if line.startswith("IMU quarter "):
                    clock.return_value = 1

            transport.send = delayed_send
            tour.exchange(transport, tour.Step("HOME", "HOME", "IMU quarter 1"))
        sleep.assert_called_once_with(0.001)

    def test_discovery_and_plan_share_quarter_cycle(self):
        transport = FakeTransport()
        self.assertTrue(tour.run(transport, emit=lambda line: None))
        tilts = [line for line in transport.sent if line.startswith("IMU quarter ")]
        self.assertEqual(tilts, [f"IMU quarter {index % 4}" for index in range(len(tilts))])

    def test_tour_rejects_prefixed_errors_during_heartbeat(self):
        transport = FakeTransport()
        transport.pending = ["I (4) console: OTHER_ERR refused"]
        with self.assertRaisesRegex(tour.TourError, "OTHER_ERR refused"):
            tour.exchange(transport, tour.Step("IMU release"))
