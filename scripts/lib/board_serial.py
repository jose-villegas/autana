"""Modem-line policy for opening a running USB Serial/JTAG board."""
import errno
import os


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
