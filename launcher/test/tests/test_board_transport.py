"""Board transport contracts without opening a physical port."""
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/test/device"))
import console_pytest

PYTEST_TIMEOUT_S = 60


class BoardTransportTests(unittest.TestCase):
    def test_qemu_waits_for_boot_before_probing(self):
        dut = mock.Mock(spec=["expect", "write", "qemu"])
        dut.expect.side_effect = [[line] for line in (
            b"rst:0xc (RTC_SW_CPU_RST),boot:0x8\n", b"RESET_REASON=POWERON\n",
            console_pytest.qemu_run.LISTENING.encode(), b"BUILD_ID=host\n",
            b"IMU_OK quarter=0 quarters=4 sample_ms=10\n")]
        original_expect = dut.expect
        fixture = console_pytest.console.__wrapped__(dut)
        next(fixture)
        self.assertEqual(dut.write.call_args_list[0], mock.call(console_pytest.BUILD_ID_REQUEST))
        self.assertEqual(original_expect.call_count, 5)
        fixture.close()

    def test_board_boot_lines_fail_the_initial_probe(self):
        for line in (b"ESP-ROM:esp32s3-20210327\n",
                     b"rst:0xc (RTC_SW_CPU_RST),boot:0x8\n",
                     b"RESET_REASON=POWERON\n"):
            with self.subTest(line=line):
                dut = mock.Mock(spec=["expect", "write", "serial"])
                dut.serial = object()
                dut.expect.return_value = [line]
                fixture = console_pytest.console.__wrapped__(dut)
                with self.assertRaisesRegex(AssertionError, "crash or reset"):
                    next(fixture)
                self.assertEqual(dut.write.call_args_list[0], mock.call(console_pytest.BUILD_ID_REQUEST))

    def test_board_reset_after_startup_fails_the_exchange(self):
        dut = mock.Mock(spec=["expect", "write", "serial"])
        dut.serial = object()
        original_expect = dut.expect
        original_expect.side_effect = [[b"BUILD_ID=host\n"],
                                       [b"IMU_OK quarter=0 quarters=4 sample_ms=10\n"]]
        fixture = console_pytest.console.__wrapped__(dut)
        shell = next(fixture)
        original_expect.side_effect = None
        for pattern in console_pytest.FORBIDDEN:
            for line in (b"ESP-ROM:esp32s3-20210327\n", b"RESET_REASON=POWERON\n",
                         b"rst:0xc (RTC_SW_CPU_RST),boot:0x8\n", b"assert failed: host\n"):
                if pattern.search(line):
                    original_expect.return_value = [line]
                    with self.assertRaisesRegex(AssertionError, "crash or reset"):
                        shell.exchange(console_pytest.BUILD_ID_REQUEST, console_pytest.BUILD_ID_RE.pattern)
        fixture.close()

    def test_session_port_with_per_test_wrappers(self):
        self.run_session("idf,serial")

    def test_qemu_keeps_plugin_transport(self):
        self.run_session("idf,qemu")

    def run_session(self, services):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "conftest.py").write_text(textwrap.dedent('''
                import queue
                from types import SimpleNamespace
                import pytest
                import serial
                from pytest_embedded_serial import Serial

                import runpy
                pytest_plugins = runpy.run_path(CONFTEST_PATH)["pytest_plugins"]
                events = []

                @pytest.fixture(autouse=True)
                def windows_modem_policy(monkeypatch):
                    import board_serial
                    monkeypatch.setattr(board_serial, "os", SimpleNamespace(name="nt"))

                @pytest.fixture
                def qemu_prog_path():
                    return None

                @pytest.fixture
                def qemu_extra_args():
                    return None

                class FakeSerial(serial.SerialBase):
                    def __setattr__(self, name, value):
                        if name in ("dtr", "rts"):
                            events.append((name, value))
                        super().__setattr__(name, value)

                    def _reconfigure_port(self, **kwargs):
                        pass

                    def _update_dtr_state(self):
                        pass

                    def _update_rts_state(self):
                        pass

                    def open(self):
                        events.append(("open", self.dtr, self.rts))
                        self.is_open = True

                    def close(self):
                        if self.is_open:
                            events.append(("close", self.dtr, self.rts))
                            self.is_open = False

                    def read_all(self):
                        return b""

                serial.Serial = FakeSerial

                def serial_for_url(port, **kwargs):
                    connection = FakeSerial(**kwargs)
                    connection.port = port
                    connection.open()
                    return connection

                serial.serial_for_url = serial_for_url

                @pytest.fixture
                def msg_queue():
                    return queue.Queue()

                @pytest.fixture
                def app():
                    return SimpleNamespace()

                @pytest.fixture
                def _fixture_classes_and_options(msg_queue, embedded_services):
                    if "qemu" in embedded_services:
                        return SimpleNamespace(classes={}, kwargs={})
                    return SimpleNamespace(classes={"serial": Serial},
                        kwargs={"serial": {"port": "FAKE", "baud": Serial.DEFAULT_BAUDRATE,
                                           "msg_queue": msg_queue}})

                @pytest.fixture(scope="session", autouse=True)
                def verify_closed(request):
                    yield
                    if "qemu" in request.config.getoption("embedded_services"):
                        assert events == []
                    else:
                        assert [event for event in events if event[0] == "close"] == [("close", False, False)]
            ''').replace("CONFTEST_PATH", repr(str(ROOT / "launcher/test/device/conftest.py"))))
            (path / "test_session.py").write_text(textwrap.dedent('''
                from conftest import events

                connections = []
                wrappers = []

                def test_first(serial):
                    connections.append(serial.proc)
                    wrappers.append(serial)
                    assert [event for event in events if event[0] == "open"] == [("open", False, False)]
                    opened = events.index(("open", False, False))
                    assert ("dtr", False) in events[:opened]
                    assert ("rts", False) in events[:opened]

                def test_second(serial):
                    assert serial is not wrappers[0]
                    assert serial.proc is connections[0]
                    assert serial.proc.is_open
                    assert len([event for event in events if event[0] == "open"]) == 1
                    assert not any(event[0] == "close" for event in events)

                def test_third(serial):
                    assert serial.proc is connections[0]
                    assert len([event for event in events if event[0] == "open"]) == 1
            '''))
            if "qemu" in services:
                (path / "test_session.py").write_text('''
def test_plugin_transport(serial):
    assert serial is None
''')
            import os
            env = dict(os.environ, PYTHONPATH=str(ROOT / "launcher/test/device"))
            result = subprocess.run([sys.executable, "-m", "pytest", "-q", str(path),
                                     "--embedded-services=" + services],
                                    cwd=path, env=env, capture_output=True, text=True, timeout=PYTEST_TIMEOUT_S)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
