"""Session-owned board port with per-test pytest-embedded readers and logs."""
from pathlib import Path
import sys

import pytest
from pytest_embedded.dut_factory import serial_gn
from pytest_embedded.plugin import multi_dut_generator_fixture
from pytest_embedded_serial import Serial
import serial as pyserial

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/lib"))
import board_serial


class BorrowedSerial(Serial):
    def close(self):
        self.stop_redirect_thread()
        for port in self.ports_to_occupy:
            self.occupied_ports.pop(port, None)


@pytest.fixture(scope="session")
def board_ports():
    ports = {}
    try:
        yield ports
    finally:
        for connection in ports.values():
            connection.close()


@pytest.fixture
@multi_dut_generator_fixture
def serial(embedded_services, _fixture_classes_and_options, msg_queue, app, board_ports):
    options = _fixture_classes_and_options
    if set((embedded_services or "").split(",")) != {"idf", "serial"}:
        return serial_gn(options, msg_queue, app)
    kwargs = dict(options.kwargs["serial"])
    port = kwargs["port"]
    baud = kwargs.get("baud") or Serial.DEFAULT_BAUDRATE
    key = (port, baud)
    if key not in board_ports:
        connection = pyserial.Serial(port=None, baudrate=baud)
        connection.port = port
        board_serial.before_open(connection)
        try:
            connection.open()
            board_serial.after_open(connection)
        except BaseException:
            connection.close()
            raise
        board_ports[key] = connection
    kwargs["port"] = board_ports[key]
    return BorrowedSerial(**kwargs)
