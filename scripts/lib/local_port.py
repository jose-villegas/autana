"""A localhost TCP port nothing holds."""
import socket


def free_port():
    """A port the system had free on 127.0.0.1 when asked; another process may take it before the caller binds."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]
