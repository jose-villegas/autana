"""Serial I/O policy for USB Serial/JTAG boards."""
import errno
import os

READ_BYTES = 4096


def read_ready(connection):
    """What the port holds now, up to READ_BYTES, or the first byte to arrive
    within the read timeout. pyserial returns a short read only when the
    timeout runs out, so asking for more than is waiting holds every read
    for the whole timeout while a board sends less than that."""
    return connection.read(min(max(connection.in_waiting, 1), READ_BYTES))


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
