"""Serial I/O policy for USB Serial/JTAG boards."""
import errno
import os

READ_BYTES = 4096


def read_ready(connection, limit=READ_BYTES):
    """Read available bytes up to limit, or wait for one byte if empty.

    Requesting more bytes than are available can stall until the read timeout.
    """
    return connection.read(min(max(connection.in_waiting, 1), limit))


def before_open(connection):
    """Avoid DTR low with RTS high, which resets USB Serial/JTAG."""
    if os.name == "nt":
        connection.dtr = connection.rts = False
    else:
        connection.exclusive = True


def after_open(connection):
    """POSIX opens with both lines raised; lower RTS before DTR."""
    if os.name != "nt":
        try:
            connection.rts = False
            connection.dtr = False
        except OSError as error:
            # PTYs have no modem lines.
            if error.errno not in (errno.EINVAL, errno.ENOTTY):
                raise
