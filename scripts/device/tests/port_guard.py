"""Keeps tool tests off the real boards.

Importing this refuses pyserial's port open and USB listing in this process,
and the run exits non-zero if either was attempted, even where the code under
test swallowed the refusal. A test reaches a board only through a fake it
hands the code under test: a FakeConnection, a patched opener, a stand-in
serial module, or a pseudo-terminal of its own (fake_port). A child process a
test starts is not guarded. Every test module that imports device imports
this first (isolation does it for the device and autana suites).
"""

import atexit
import contextlib
import os
import sys


class RealPortTouched(BaseException):
    """A test reached a real serial port. Not an OSError, which the loops
    that wait out a busy or re-enumerating port catch and retry."""


violations = []
fake_ports = set()


def refuse(what):
    violations.append(what)
    raise RealPortTouched("a test " + what + "; hand the code under test a fake")


def refuse_real_ports():
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        return
    real_open = serial.Serial.open

    def open_port(self):
        if self.port not in fake_ports:
            refuse("opened the real serial port " + str(self.port))
        real_open(self)

    def comports(*unused, **unused_keywords):
        refuse("listed the real USB serial ports")

    serial.Serial.open = open_port
    list_ports.comports = comports


@contextlib.contextmanager
def fake_port():
    """A pseudo-terminal: (the test's end as a file descriptor, the name the
    code under test opens as its port). Nothing reads the test's end unless
    the test does, as with a board that stopped reading its console."""
    import pty
    ours, theirs = pty.openpty()
    name = os.ttyname(theirs)
    fake_ports.add(name)
    try:
        yield ours, name
    finally:
        fake_ports.discard(name)
        os.close(theirs)
        os.close(ours)


def finish():
    if violations:
        sys.stderr.write("FAIL: the test run touched a real serial port:\n  " +
                         "\n  ".join(sorted(set(violations))) + "\n")
        sys.stderr.flush()
        os._exit(1)


refuse_real_ports()
atexit.register(finish)
