"""Scripted serial handles for device command tests; no port is opened."""

import subprocess


class FakeConnection:
    def __init__(self, chunks=()):
        self.chunks = list(chunks)
        self.writes = []

    def read(self, unused_size):
        return self.chunks.pop(0) if self.chunks else b""

    def write(self, data):
        self.writes.append(data)

    def close(self):
        pass

    def flush(self):
        pass

    def reset_input_buffer(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        return False


class Replies(FakeConnection):
    """One reply, with a writable pending buffer for console subclasses."""

    def __init__(self, data):
        super().__init__()
        self.data = data

    def read(self, unused_size):
        data, self.data = self.data, b""
        return data


class FakeUsbBoard:
    """One board at pyserial's boundary, in the order measured on the board:
    a reset unlists its port after `leaves`, lists it after `returns` under
    the next name, and permits opens after `opens`. Old handles die. DTR low
    with RTS high resets USB Serial/JTAG. Each handle reads boot_line once."""

    boot_line = b""

    def __init__(self, serial_number, names, leaves=0.22, returns=1.02, opens=1.10, busy_at=0.16):
        self.serial_number = serial_number
        self.names = list(names)
        self.port = self.names.pop(0)
        self.leaves, self.returns = leaves, returns
        self.opens, self.busy_at = opens, busy_at
        self.open_at = 0
        self.blocked_at = None
        self.boot = 0
        self.reset_at = None
        self.resets = 0

    def settle(self, now):
        if self.reset_at is not None and now >= self.reset_at + self.returns:
            self.reset_at = None
            self.boot += 1
            if self.names:
                self.port = self.names.pop(0)

    def listed(self, now):
        self.settle(now)
        return self.reset_at is None or now < self.reset_at + self.leaves

    def reset(self, now):
        self.settle(now)
        if self.reset_at is None:
            self.reset_at = now
            self.open_at = now + self.opens
            self.blocked_at = now + self.busy_at
            self.resets += 1

    def open(self, port, dtr, rts, now):
        """The boot a new handle belongs to; FileNotFoundError when `port` is
        not on the bus, as pyserial reports a port that left."""
        if (not self.listed(now) or port != self.port
                or (self.blocked_at is not None and self.blocked_at <= now < self.open_at)):
            raise FileNotFoundError(2, "could not open port " + str(port))
        boot = self.boot
        if not dtr and rts:
            self.reset(now)
        return boot

    def alive(self, boot, now):
        return self.listed(now) and self.reset_at is None and boot == self.boot

    def serial_module(self, clock, sleep=lambda seconds: None, platform="nt"):
        """A stand-in for the `serial` module, every port on this board."""
        board = self

        class Serial:
            def __init__(self, port=None, **unused):
                self.boot = None
                self.port, self.dtr, self.rts = port, True, True
                self.boot, self.unread = None, b""
                if port is not None:
                    self.open()

            def __setattr__(self, name, value):
                object.__setattr__(self, name, value)
                if name in ("dtr", "rts") and getattr(self, "boot", None) is not None:
                    board.open(self.port, self.dtr, self.rts, clock())

            def open(self):
                if platform == "posix":
                    board.open(self.port, self.dtr, True, clock())
                self.boot = board.open(self.port, self.dtr, self.rts, clock())
                self.unread = board.boot_line

            def read(self, unused_size):
                sleep(self.timeout)
                if not board.alive(self.boot, clock()):
                    raise OSError(22, "ClearCommError failed")
                data, self.unread = self.unread, b""
                return data

            def close(self):
                self.boot = None

            def __enter__(self):
                return self

            def __exit__(self, *unused):
                self.close()
                return False

        class SerialTimeoutException(OSError):
            pass

        return type("serial", (), {"Serial": Serial,
                                   "SerialTimeoutException": SerialTimeoutException})

    def esptool(self, clock, sleep, startup=0.2, status=0):
        """subprocess.run for `esptool ... --after hard_reset chip_id`: a new
        process opens the port about `startup` seconds later, and the reset
        on its way out takes the port off the bus."""
        def run(command, check=False, **unused):
            sleep(startup)
            port = command[command.index("-p") + 1]
            try:
                self.open(port, False, False, clock())
            except FileNotFoundError:
                if check:
                    raise subprocess.CalledProcessError(2, command)
                return subprocess.CompletedProcess(command, 2)
            self.reset(clock())
            result = subprocess.CompletedProcess(command, status)
            if check:
                result.check_returncode()
            return result
        return run
